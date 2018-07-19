//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2018, Image Engine Design Inc. All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are
//  met:
//
//      * Redistributions of source code must retain the above
//        copyright notice, this list of conditions and the following
//        disclaimer.
//
//      * Redistributions in binary form must reproduce the above
//        copyright notice, this list of conditions and the following
//        disclaimer in the documentation and/or other materials provided with
//        the distribution.
//
//      * Neither the name of John Haddon nor the names of
//        any other contributors to this software may be used to endorse or
//        promote products derived from this software without specific prior
//        written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
//  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////

#include "GafferScene/RenderController.h"

#include "GafferScene/SceneAlgo.h"

#include "Gaffer/ParallelAlgo.h"

#include "IECoreScene/CurvesPrimitive.h"
#include "IECoreScene/Transform.h"
#include "IECoreScene/VisibleRenderable.h"

#include "IECore/NullObject.h"

#include "boost/algorithm/string/predicate.hpp"
#include "boost/bind.hpp"

#include "tbb/task.h"

using namespace std;
using namespace Imath;
using namespace IECore;
using namespace IECoreScene;
using namespace Gaffer;
using namespace GafferScene;
using namespace GafferScene::RendererAlgo;

//////////////////////////////////////////////////////////////////////////
// Private utilities
//////////////////////////////////////////////////////////////////////////

namespace
{

InternedString g_openGLRendererName( "OpenGL" );

InternedString g_cameraGlobalName( "option:render:camera" );

InternedString g_visibleAttributeName( "scene:visible" );
InternedString g_setsAttributeName( "sets" );
InternedString g_rendererContextName( "scene:renderer" );

bool visible( const CompoundObject *attributes )
{
	const IECore::BoolData *d = attributes->member<IECore::BoolData>( g_visibleAttributeName );
	return d ? d->readable() : true;
}

bool cameraGlobalsChanged( const CompoundObject *globals, const CompoundObject *previousGlobals )
{
	if( !previousGlobals )
	{
		return true;
	}
	CameraPtr camera1 = new Camera;
	CameraPtr camera2 = new Camera;
	RendererAlgo::applyCameraGlobals( camera1.get(), globals );
	RendererAlgo::applyCameraGlobals( camera2.get(), previousGlobals );

	return *camera1 != *camera2;
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// Internal implementation details
//////////////////////////////////////////////////////////////////////////

// Represents a location in the Gaffer scene as specified to the
// renderer. We use this to build up a persistent representation of
// the scene which we can traverse to perform selective updates to
// only the changed locations. A Renderer's representation of the
// scene contains only a flat list of objects, whereas the SceneGraph
// maintains the original hierarchy, providing the means of flattening
// attribute and transform state for passing to the renderer. Calls
// to update() are made from a threaded scene traversal performed by
// SceneGraphUpdateTask.
class RenderController::SceneGraph
{

	public :

		// We store separate scene graphs for
		// objects which are classified differently
		// by the renderer. This lets us output
		// lights and cameras prior to the
		// rest of the scene, which may be a
		// requirement of some renderer backends.
		enum Type
		{
			CameraType = 0,
			LightType = 1,
			ObjectType = 2,
			FirstType = CameraType,
			LastType = ObjectType,
			NoType = LastType + 1
		};

		enum Component
		{
			NoComponent = 0,
			BoundComponent = 1,
			TransformComponent = 2,
			AttributesComponent = 4,
			ObjectComponent = 8,
			ChildNamesComponent = 16,
			GlobalsComponent = 32,
			SetsComponent = 64,
			RenderSetsComponent = 128, // Sets prefixed with "render:"
			ExpansionComponent = 256,
			AllComponents = BoundComponent | TransformComponent | AttributesComponent | ObjectComponent | ChildNamesComponent | GlobalsComponent | SetsComponent | RenderSetsComponent | ExpansionComponent
		};

		// Constructs the root of the scene graph.
		// Children are constructed using updateChildren().
		SceneGraph()
			:	m_parent( nullptr ), m_fullAttributes( new CompoundObject )
		{
			clear();
		}

		~SceneGraph()
		{
			clear();
		}

		const InternedString &name() const
		{
			return m_name;
		}

		// Called by SceneGraphUpdateTask to update this location. Returns a bitmask
		// of the components which were changed.
		unsigned update( const ScenePlug::ScenePath &path, unsigned dirtyComponents, unsigned changedParentComponents, Type type, const RenderController *controller )
		{
			unsigned changedComponents = 0;

			// Attributes

			if( !m_parent )
			{
				// Root - get attributes from globals.
				if( dirtyComponents & GlobalsComponent )
				{
					if( updateAttributes( controller->m_globals.get() ) )
					{
						changedComponents |= AttributesComponent;
					}
				}
			}
			else
			{
				// Non-root - get attributes the standard way.
				const bool parentAttributesChanged = changedParentComponents & AttributesComponent;
				if( parentAttributesChanged || ( dirtyComponents & AttributesComponent ) )
				{
					if( updateAttributes( controller->m_scene->attributesPlug(), parentAttributesChanged ) )
					{
						changedComponents |= AttributesComponent;
					}
				}
			}

			if( !::visible( m_fullAttributes.get() ) )
			{
				clear();
				return changedComponents;
			}

			// Render Sets. We must obviously update these if
			// the sets have changed, but we also need to do an
			// update if the attributes have changed, because in
			// that case we may have overwritten the sets attribute.

			if( ( dirtyComponents & RenderSetsComponent ) || ( changedComponents & AttributesComponent ) )
			{
				if( updateRenderSets( path, controller->m_renderSets ) )
				{
					changedComponents |= AttributesComponent;
				}
			}

			// Transform

			if( ( dirtyComponents & TransformComponent ) && updateTransform( controller->m_scene->transformPlug(), changedParentComponents & TransformComponent ) )
			{
				changedComponents |= TransformComponent;
			}

			// Object

			if( ( dirtyComponents & ObjectComponent ) && updateObject( controller->m_scene->objectPlug(), type, controller->m_renderer.get(), controller->m_globals.get() ) )
			{
				changedComponents |= ObjectComponent;
			}

			if( m_objectInterface )
			{
				if( !(changedComponents & ObjectComponent) )
				{
					// Apply attribute update to old object if necessary.
					if( changedComponents & AttributesComponent )
					{
						if( !m_objectInterface->attributes( attributesInterface( controller->m_renderer.get() ) ) )
						{
							// Failed to apply attributes - must replace entire object.
							m_objectHash = MurmurHash();
							if( updateObject( controller->m_scene->objectPlug(), type, controller->m_renderer.get(), controller->m_globals.get() ) )
							{
								changedComponents |= ObjectComponent;
							}
						}
					}
				}

				// If the transform has changed, or we have an entirely new object,
				// the apply the transform.
				if( changedComponents & ( ObjectComponent | TransformComponent ) )
				{
					m_objectInterface->transform( m_fullTransform );
				}
			}

			// Children

			if( ( dirtyComponents & ChildNamesComponent ) && updateChildren( controller->m_scene->childNamesPlug() ) )
			{
				changedComponents |= ChildNamesComponent;
			}

			// Expansion

			if( ( dirtyComponents & ExpansionComponent ) && updateExpansion( path, controller->m_expandedPaths, controller->m_minimumExpansionDepth ) )
			{
				changedComponents |= ExpansionComponent;
			}

			if(
				( changedComponents & ( ExpansionComponent | ChildNamesComponent ) ) ||
				( dirtyComponents & BoundComponent )
			)
			{
				// Create bounding box if needed
				if( !m_expanded && m_children.size() )
				{
					const Box3f bound = controller->m_scene->boundPlug()->getValue();
					IECoreScene::CurvesPrimitivePtr boundCurves = IECoreScene::CurvesPrimitive::createBox( bound );

					std::string boundName;
					ScenePlug::pathToString( path, boundName );
					boundName += "/__unexpandedChildren__";

					if( controller->m_renderer->name() != g_openGLRendererName )
					{
						// See comments in `updateObject()`.
						m_boundInterface = nullptr;
					}

					m_boundInterface = controller->m_renderer->object( boundName, boundCurves.get(), controller->m_boundAttributes.get() );
					m_boundInterface->transform( m_fullTransform );
				}
				else
				{
					m_boundInterface = nullptr;
				}
			}
			else if( m_boundInterface && ( changedComponents & TransformComponent ) )
			{
				// Apply new transform to existing bounding box
				m_boundInterface->transform( m_fullTransform );
			}

			m_cleared = false;

			return changedComponents;
		}

		bool expanded() const
		{
			return m_expanded;
		}

		const std::vector<SceneGraph *> &children()
		{
			return m_children;
		}

		// Invalidates this location, removing any resources it
		// holds in the renderer, and clearing all children. This is
		// used to "remove" a location without having to delete it
		// from the children() of its parent. We avoid the latter
		// because it would involve some unwanted locking - we
		// process children in parallel, and therefore want to avoid
		// child updates having to write to the parent.
		void clear()
		{
			clearChildren();
			clearObject();
			m_attributesHash = m_transformHash = m_childNamesHash = IECore::MurmurHash();
			m_cleared = true;
			m_expanded = false;
			m_boundInterface = nullptr;
		}

		// Returns true if the location has not been finalised
		// since the last call to clear() - ie that it is not
		// in a valid state.
		bool cleared()
		{
			return m_cleared;
		}

	private :

		SceneGraph( const InternedString &name, const SceneGraph *parent )
			:	m_name( name ), m_parent( parent ), m_fullAttributes( new CompoundObject )
		{
			clear();
		}

		// Returns true if the attributes changed.
		bool updateAttributes( const CompoundObjectPlug *attributesPlug, bool parentAttributesChanged )
		{
			assert( m_parent );

			const IECore::MurmurHash attributesHash = attributesPlug->hash();
			if( attributesHash == m_attributesHash && !parentAttributesChanged )
			{
				return false;
			}

			ConstCompoundObjectPtr attributes = attributesPlug->getValue( &attributesHash );
			CompoundObject::ObjectMap &fullAttributes = m_fullAttributes->members();
			fullAttributes = m_parent->m_fullAttributes->members();
			for( CompoundObject::ObjectMap::const_iterator it = attributes->members().begin(), eIt = attributes->members().end(); it != eIt; ++it )
			{
				fullAttributes[it->first] = it->second;
			}

			m_attributesInterface = nullptr; // Will be updated lazily in attributesInterface()
			m_attributesHash = attributesHash;

			return true;
		}

		// As above, but for use at the root.
		bool updateAttributes( const CompoundObject *globals )
		{
			assert( !m_parent );

			ConstCompoundObjectPtr globalAttributes = GafferScene::SceneAlgo::globalAttributes( globals );
			if( m_fullAttributes && *m_fullAttributes == *globalAttributes )
			{
				return false;
			}

			m_fullAttributes->members() = globalAttributes->members();
			m_attributesInterface = nullptr;

			return true;
		}

		bool updateRenderSets( const ScenePlug::ScenePath &path, const RendererAlgo::RenderSets &renderSets )
		{
			m_fullAttributes->members()[g_setsAttributeName] = boost::const_pointer_cast<InternedStringVectorData>(
				renderSets.setsAttribute( path )
			);
			m_attributesInterface = nullptr;
			return true;
		}

		IECoreScenePreview::Renderer::AttributesInterface *attributesInterface( IECoreScenePreview::Renderer *renderer )
		{
			if( !m_attributesInterface )
			{
				m_attributesInterface = renderer->attributes( m_fullAttributes.get() );
			}
			return m_attributesInterface.get();
		}

		// Returns true if the transform changed.
		bool updateTransform( const M44fPlug *transformPlug, bool parentTransformChanged )
		{
			const IECore::MurmurHash transformHash = transformPlug->hash();
			if( transformHash == m_transformHash && !parentTransformChanged )
			{
				return false;
			}

			const M44f transform = transformPlug->getValue( &transformHash );
			if( m_parent )
			{
				m_fullTransform = transform * m_parent->m_fullTransform;
			}
			else
			{
				m_fullTransform = transform;
			}

			m_transformHash = transformHash;
			return true;
		}

		// Returns true if the object changed.
		bool updateObject( const ObjectPlug *objectPlug, Type type, IECoreScenePreview::Renderer *renderer, const IECore::CompoundObject *globals )
		{
			const bool hadObjectInterface = static_cast<bool>( m_objectInterface );
			if( type == NoType )
			{
				m_objectInterface = nullptr;
				return hadObjectInterface;
			}

			const IECore::MurmurHash objectHash = objectPlug->hash();
			if( objectHash == m_objectHash )
			{
				return false;
			}

			IECore::ConstObjectPtr object = objectPlug->getValue( &objectHash );
			m_objectHash = objectHash;

			const IECore::NullObject *nullObject = runTimeCast<const IECore::NullObject>( object.get() );
			if( (type != LightType) && nullObject )
			{
				m_objectInterface = nullptr;
				return hadObjectInterface;
			}

			if( renderer->name() != g_openGLRendererName )
			{
				// Delete our current object interface before we potentially
				// create a new one. This is essential for renderer backends
				// which rely on object names being unique (typically because
				// they use them as handles in the renderer they connect to).
				// We avoid doing this for the OpenGL renderer though, because
				// destroying the object before its replacement is ready can
				// lead to the object flickering during progressive updates
				// in Gaffer's viewport (the OpenGL renderer is designed such
				// that it can draw concurrently with the updates we make,
				// whereas other renderers must wait for all edits to be complete
				// first).
				//
				/// \todo Consider ways of redesigning the Renderer API so this
				/// is cleaner. Should there be an atomic way of swapping an
				/// ObjectInterface? Or a way of updating geometry without creating
				/// a new object? Perhaps the latter could allow a smart backend to make
				/// more minimal edits?
				m_objectInterface = nullptr;
			}

			std::string name;
			ScenePlug::pathToString( Context::current()->get<vector<InternedString> >( ScenePlug::scenePathContextName ), name );
			if( type == CameraType )
			{
				if( const IECoreScene::Camera *camera = runTimeCast<const IECoreScene::Camera>( object.get() ) )
				{
					IECoreScene::CameraPtr cameraCopy = camera->copy();
					RendererAlgo::applyCameraGlobals( cameraCopy.get(), globals );
					m_objectInterface = renderer->camera( name, cameraCopy.get(), attributesInterface( renderer ) );
				}
				else
				{
					m_objectInterface = nullptr;
				}
			}
			else if( type == LightType )
			{
				m_objectInterface = renderer->light( name, nullObject ? nullptr : object.get(), attributesInterface( renderer ) );
			}
			else
			{
				m_objectInterface = renderer->object( name, object.get(), attributesInterface( renderer ) );
			}

			return true;
		}

		void clearObject()
		{
			m_objectInterface = nullptr;
			m_objectHash = MurmurHash();
		}

		bool updateExpansion( const ScenePlug::ScenePath &path, const IECore::PathMatcher &expandedPaths, size_t minimumExpansionDepth )
		{
			const bool expanded = ( minimumExpansionDepth >= path.size() ) || ( expandedPaths.match( path ) & PathMatcher::ExactMatch );
			if( expanded == m_expanded )
			{
				return false;
			}
			m_expanded = expanded;
			return true;
		}

		// Ensures that children() contains a child for every name specified
		// by childNamesPlug(). This just ensures that the children exist - they
		// will be subsequently be updated in parallel by the SceneGraphUpdateTask.
		bool updateChildren( const InternedStringVectorDataPlug *childNamesPlug )
		{
			const IECore::MurmurHash childNamesHash = childNamesPlug->hash();
			if( childNamesHash == m_childNamesHash )
			{
				return false;
			}

			IECore::ConstInternedStringVectorDataPtr childNamesData = childNamesPlug->getValue( &childNamesHash );
			const std::vector<IECore::InternedString> &childNames = childNamesData->readable();
			if( !existingChildNamesValid( childNames ) )
			{
				clearChildren();
				for( std::vector<IECore::InternedString>::const_iterator it = childNames.begin(), eIt = childNames.end(); it != eIt; ++it )
				{
					SceneGraph *child = new SceneGraph( *it, this );
					m_children.push_back( child );
				}
			}
			return true;
		}

		void clearChildren()
		{
			for( std::vector<SceneGraph *>::const_iterator it = m_children.begin(), eIt = m_children.end(); it != eIt; ++it )
			{
				delete *it;
			}
			m_children.clear();
		}

		bool existingChildNamesValid( const vector<IECore::InternedString> &childNames ) const
		{
			if( m_children.size() != childNames.size() )
			{
				return false;
			}
			for( size_t i = 0, e = childNames.size(); i < e; ++i )
			{
				if( m_children[i]->m_name != childNames[i] )
				{
					return false;
				}
			}
			return true;
		}

		IECore::InternedString m_name;

		const SceneGraph *m_parent;

		IECore::MurmurHash m_objectHash;
		IECoreScenePreview::Renderer::ObjectInterfacePtr m_objectInterface;

		IECore::MurmurHash m_attributesHash;
		IECore::CompoundObjectPtr m_fullAttributes;
		IECoreScenePreview::Renderer::AttributesInterfacePtr m_attributesInterface;

		IECore::MurmurHash m_transformHash;
		Imath::M44f m_fullTransform;

		IECore::MurmurHash m_childNamesHash;
		std::vector<SceneGraph *> m_children;

		IECoreScenePreview::Renderer::ObjectInterfacePtr m_boundInterface;
		bool m_expanded;

		bool m_cleared;

};

// TBB task used to perform multithreaded updates on our SceneGraph.
class RenderController::SceneGraphUpdateTask : public tbb::task
{

	public :

		SceneGraphUpdateTask(
			const RenderController *controller,
			SceneGraph *sceneGraph,
			SceneGraph::Type sceneGraphType,
			unsigned dirtyComponents,
			unsigned changedParentComponents,
			const Context *context,
			const ScenePlug::ScenePath &scenePath,
			const ProgressCallback &callback
		)
			:	m_controller( controller ),
				m_sceneGraph( sceneGraph ),
				m_sceneGraphType( sceneGraphType ),
				m_dirtyComponents( dirtyComponents ),
				m_changedParentComponents( changedParentComponents ),
				m_context( context ),
				m_scenePath( scenePath ),
				m_callback( callback )
		{
		}

		task *execute() override
		{

			// Figure out if this location belongs in the type
			// of scene graph we're constructing. If it doesn't
			// belong, and neither do any of its descendants,
			// we can just early out.

			const unsigned sceneGraphMatch = this->sceneGraphMatch();
			if( !( sceneGraphMatch & ( IECore::PathMatcher::ExactMatch | IECore::PathMatcher::DescendantMatch ) ) )
			{
				m_sceneGraph->clear();
				return nullptr;
			}

			if( m_sceneGraph->cleared() )
			{
				// We cleared this location in the past, but now
				// want it. So we need to start from scratch, and
				// update everything.
				m_dirtyComponents = SceneGraph::AllComponents;
			}

			// Set up a context to compute the scene at the right
			// location.

			ScenePlug::PathScope pathScope( m_context, m_scenePath );

			// Update the scene graph at this location.

			unsigned changedComponents = m_sceneGraph->update(
				m_scenePath,
				m_dirtyComponents,
				m_changedParentComponents,
				sceneGraphMatch & IECore::PathMatcher::ExactMatch ? m_sceneGraphType : SceneGraph::NoType,
				m_controller
			);

			if( changedComponents && m_callback )
			{
				m_callback( BackgroundTask::Running );
			}

			// Spawn subtasks to apply updates to each child.

			const std::vector<SceneGraph *> &children = m_sceneGraph->children();
			if( m_sceneGraph->expanded() && children.size() )
			{
				set_ref_count( 1 + children.size() );

				ScenePlug::ScenePath childPath = m_scenePath;
				childPath.push_back( IECore::InternedString() ); // space for the child name
				for( std::vector<SceneGraph *>::const_iterator it = children.begin(), eIt = children.end(); it != eIt; ++it )
				{
					childPath.back() = (*it)->name();
					SceneGraphUpdateTask *t = new( allocate_child() ) SceneGraphUpdateTask( m_controller, *it, m_sceneGraphType, m_dirtyComponents, changedComponents, m_context, childPath, m_callback );
					spawn( *t );
				}

				wait_for_all();
			}
			else
			{
				for( auto &child : children )
				{
					child->clear();
				}
			}

			return nullptr;
		}

	private :

		const ScenePlug *scene()
		{
			return m_controller->m_scene.get();
		}

		/// \todo Fast path for when sets were not dirtied.
		const unsigned sceneGraphMatch() const
		{
			switch( m_sceneGraphType )
			{
				case SceneGraph::CameraType :
					return m_controller->m_renderSets.camerasSet().match( m_scenePath );
				case SceneGraph::LightType :
					return m_controller->m_renderSets.lightsSet().match( m_scenePath );
				case SceneGraph::ObjectType :
				{
					unsigned m = m_controller->m_renderSets.lightsSet().match( m_scenePath ) |
					             m_controller->m_renderSets.camerasSet().match( m_scenePath );
					if( m & IECore::PathMatcher::ExactMatch )
					{
						return IECore::PathMatcher::AncestorMatch | IECore::PathMatcher::DescendantMatch;
					}
					else
					{
						return IECore::PathMatcher::EveryMatch;
					}
				}
				default :
					return IECore::PathMatcher::NoMatch;
			}
		}

		const RenderController *m_controller;
		SceneGraph *m_sceneGraph;
		SceneGraph::Type m_sceneGraphType;
		unsigned m_dirtyComponents;
		unsigned m_changedParentComponents;
		const Context *m_context;
		ScenePlug::ScenePath m_scenePath;
		const ProgressCallback &m_callback;

};

//////////////////////////////////////////////////////////////////////////
// RenderController
//////////////////////////////////////////////////////////////////////////

RenderController::RenderController( const ConstScenePlugPtr &scene, const Gaffer::ConstContextPtr &context, const IECoreScenePreview::RendererPtr &renderer )
	:	m_renderer( renderer ),
		m_minimumExpansionDepth( 0 ),
		m_updateRequired( false ),
		m_updateRequested( false ),
		m_dirtyComponents( SceneGraph::AllComponents ),
		m_globals( new CompoundObject )
{
	for( int i = SceneGraph::FirstType; i <= SceneGraph::LastType; ++i )
	{
		m_sceneGraphs.push_back( unique_ptr<SceneGraph>( new SceneGraph ) );
	}

	CompoundObjectPtr boundAttributes = new CompoundObject;
	boundAttributes->members()["gl:curvesPrimitive:useGLLines"] = new BoolData( true );
	boundAttributes->members()["gl:primitive:solid"] = new BoolData( false );
	boundAttributes->members()["gl:primitive:wireframe"] = new BoolData( true );
	boundAttributes->members()["gl:primitive:wireframeColor"] = new Color4fData( Color4f( 0.2f, 0.2f, 0.2f, 1.0f ) );
	m_boundAttributes = m_renderer->attributes( boundAttributes.get() );

	setScene( scene );
	setContext( context );
}

RenderController::~RenderController()
{
	// Cancel background task before the things it relies
	// on are destroyed.
	cancelBackgroundTask();
	// Drop references to ObjectInterfaces before the renderer
	// is destroyed.
	m_renderer->pause();
	m_sceneGraphs.clear();
	m_defaultCamera.reset();
}

IECoreScenePreview::Renderer *RenderController::renderer()
{
	return m_renderer.get();
}

void RenderController::setScene( const ConstScenePlugPtr &scene )
{
	if( scene == m_scene )
	{
		return;
	}

	const Node *node = scene->node();
	if( !node )
	{
		throw Exception( "Scene must belong to a Node" );
	}

	cancelBackgroundTask();

	m_scene = scene;
	m_plugDirtiedConnection = const_cast<Node *>( node )->plugDirtiedSignal().connect(
		boost::bind( &RenderController::plugDirtied, this, ::_1 )
	);

	m_dirtyComponents |= SceneGraph::AllComponents;
	requestUpdate();
}

const ScenePlug *RenderController::getScene() const
{
	return m_scene.get();
}

void RenderController::setContext( const Gaffer::ConstContextPtr &context )
{
	if( m_context == context )
	{
		return;
	}

	cancelBackgroundTask();

	m_context = context;
	m_contextChangedConnection = const_cast<Context *>( m_context.get() )->changedSignal().connect(
		boost::bind( &RenderController::contextChanged, this, ::_2 )
	);

	m_dirtyComponents |= SceneGraph::AllComponents;
	requestUpdate();
}

const Gaffer::Context *RenderController::getContext() const
{
	return m_context.get();
}

void RenderController::setExpandedPaths( const IECore::PathMatcher &expandedPaths )
{
	cancelBackgroundTask();

	m_expandedPaths = expandedPaths;
	m_dirtyComponents |= SceneGraph::ExpansionComponent;
	requestUpdate();
}

const IECore::PathMatcher &RenderController::getExpandedPaths() const
{
	return m_expandedPaths;
}

void RenderController::setMinimumExpansionDepth( size_t depth )
{
	if( depth == m_minimumExpansionDepth )
	{
		return;
	}

	cancelBackgroundTask();

	m_minimumExpansionDepth = depth;
	m_dirtyComponents |= SceneGraph::ExpansionComponent;
	requestUpdate();
}

size_t RenderController::getMinimumExpansionDepth() const
{
	return m_minimumExpansionDepth;
}

RenderController::UpdateRequiredSignal &RenderController::updateRequiredSignal()
{
	return m_updateRequiredSignal;
}

bool RenderController::updateRequired() const
{
	return m_updateRequired;
}

void RenderController::plugDirtied( const Gaffer::Plug *plug )
{
	if( plug == m_scene->boundPlug() )
	{
		m_dirtyComponents |= SceneGraph::BoundComponent;
	}
	else if( plug == m_scene->transformPlug() )
	{
		m_dirtyComponents |= SceneGraph::TransformComponent;
	}
	else if( plug == m_scene->attributesPlug() )
	{
		m_dirtyComponents |= SceneGraph::AttributesComponent;
	}
	else if( plug == m_scene->objectPlug() )
	{
		m_dirtyComponents |= SceneGraph::ObjectComponent;
	}
	else if( plug == m_scene->childNamesPlug() )
	{
		m_dirtyComponents |= SceneGraph::ChildNamesComponent;
	}
	else if( plug == m_scene->globalsPlug() )
	{
		m_dirtyComponents |= SceneGraph::GlobalsComponent;
	}
	else if( plug == m_scene->setPlug() )
	{
		m_dirtyComponents |= SceneGraph::SetsComponent;
	}
	else if( plug == m_scene )
	{
		requestUpdate();
	}
}

void RenderController::contextChanged( const IECore::InternedString &name )
{
	if( boost::starts_with( name.string(), "ui:" ) )
	{
		return;
	}

	cancelBackgroundTask();

	m_dirtyComponents |= SceneGraph::AllComponents;
	requestUpdate();
}

void RenderController::requestUpdate()
{
	m_updateRequired = true;
	if( !m_updateRequested )
	{
		m_updateRequested = true;
		updateRequiredSignal()( *this );
	}
}

void RenderController::update( const ProgressCallback &callback )
{
	if( !m_scene || !m_context )
	{
		return;
	}

	m_updateRequested = false;

	Context::EditableScope scopedContext( m_context.get() );
	scopedContext.set( "scene:renderer", m_renderer->name().string() );

	updateInternal( callback );
}

std::shared_ptr<Gaffer::BackgroundTask> RenderController::updateInBackground( const ProgressCallback &callback )
{
	if( !m_scene || !m_context )
	{
		return nullptr;
	}

	m_updateRequested = false;
	cancelBackgroundTask();

	Context::EditableScope scopedContext( m_context.get() );
	scopedContext.set( "scene:renderer", m_renderer->name().string() );

	m_backgroundTask = ParallelAlgo::callOnBackgroundThread(
		// Subject
		m_scene.get(),
		[this, callback] {
			updateInternal( callback );
		}
	);

	return m_backgroundTask;
}

void RenderController::updateInternal( const ProgressCallback &callback )
{
	try
	{
		bool cameraGlobalsChanged = false;
		if( m_dirtyComponents & SceneGraph::GlobalsComponent )
		{
			ConstCompoundObjectPtr globals = m_scene->globalsPlug()->getValue();
			RendererAlgo::outputOptions( globals.get(), m_globals.get(), m_renderer.get() );
			RendererAlgo::outputOutputs( globals.get(), m_globals.get(), m_renderer.get() );
			cameraGlobalsChanged = ::cameraGlobalsChanged( globals.get(), m_globals.get() );
			m_globals = globals;
		}

		if( m_dirtyComponents & SceneGraph::SetsComponent )
		{
			if( m_renderSets.update( m_scene.get() ) & RendererAlgo::RenderSets::RenderSetsChanged )
			{
				m_dirtyComponents |= SceneGraph::RenderSetsComponent;
			}
		}

		for( int i = SceneGraph::FirstType; i <= SceneGraph::LastType; ++i )
		{
			SceneGraph *sceneGraph = m_sceneGraphs[i].get();
			if( i == SceneGraph::CameraType && cameraGlobalsChanged )
			{
				// Because the globals are applied to camera objects, we must update the object whenever
				// the globals have changed, so we clear the scene graph and start again.
				sceneGraph->clear();
			}

			tbb::task_group_context taskGroupContext( tbb::task_group_context::isolated );
			SceneGraphUpdateTask *task = new( tbb::task::allocate_root( taskGroupContext ) ) SceneGraphUpdateTask(
				this, sceneGraph, (SceneGraph::Type)i, m_dirtyComponents, SceneGraph::NoComponent, Context::current(), ScenePlug::ScenePath(), callback
			);
			tbb::task::spawn_root_and_wait( *task );
		}

		if( cameraGlobalsChanged )
		{
			updateDefaultCamera();
		}

		m_dirtyComponents = SceneGraph::NoComponent;
		m_updateRequired = false;

		if( callback )
		{
			callback( BackgroundTask::Completed );
		}
	}
	catch( const IECore::Cancelled &e )
	{
		if( callback )
		{
			callback( BackgroundTask::Cancelled );
		}
		throw;
	}
	catch( ... )
	{
		// No point updating again, since it'll just repeat
		// the same error.
		m_updateRequired = false;
		if( callback )
		{
			callback( BackgroundTask::Errored );
		}
		throw;
	}
}

void RenderController::updateDefaultCamera()
{
	if( m_renderer->name() == g_openGLRendererName )
	{
		// Don't need a default camera for OpenGL, because in interactive mode the
		// renderer currently expects the camera to be provided externally.
		return;
	}

	const StringData *cameraOption = m_globals->member<StringData>( g_cameraGlobalName );
	m_defaultCamera = nullptr;
	if( cameraOption && !cameraOption->readable().empty() )
	{
		return;
	}

	CameraPtr defaultCamera = new IECoreScene::Camera;
	RendererAlgo::applyCameraGlobals( defaultCamera.get(), m_globals.get() );
	IECoreScenePreview::Renderer::AttributesInterfacePtr defaultAttributes = m_renderer->attributes( m_scene->attributesPlug()->defaultValue() );
	ConstStringDataPtr name = new StringData( "gaffer:defaultCamera" );
	m_defaultCamera = m_renderer->camera( name->readable(), defaultCamera.get(), defaultAttributes.get() );
	m_renderer->option( "camera", name.get() );
}

void RenderController::cancelBackgroundTask()
{
	if( m_backgroundTask )
	{
		m_backgroundTask->cancelAndWait();
		m_backgroundTask.reset();
	}
}
