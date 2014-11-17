##########################################################################
#
#  Copyright (c) 2012-2014, Image Engine Design Inc. All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions are
#  met:
#
#      * Redistributions of source code must retain the above
#        copyright notice, this list of conditions and the following
#        disclaimer.
#
#      * Redistributions in binary form must reproduce the above
#        copyright notice, this list of conditions and the following
#        disclaimer in the documentation and/or other materials provided with
#        the distribution.
#
#      * Neither the name of John Haddon nor the names of
#        any other contributors to this software may be used to endorse or
#        promote products derived from this software without specific prior
#        written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
#  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
#  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
#  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
#  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
#  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
#  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
#  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
#  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
#  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
#  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
##########################################################################

import IECore

import Gaffer
import GafferUI
import GafferCortex
import GafferCortexUI

class OpMode( GafferUI.BrowserEditor.Mode ) :

	def __init__( self, browser, classLoader=None ) :

		GafferUI.BrowserEditor.Mode.__init__( self, browser, splitPosition = 0.333 )

		if classLoader is not None :
			self.__classLoader = classLoader
		else :
			self.__classLoader = IECore.ClassLoader.defaultOpLoader()

	def connect( self ) :

		GafferUI.BrowserEditor.Mode.connect( self )

		self.__pathSelectedConnection = self.browser().pathChooser().pathListingWidget().pathSelectedSignal().connect( Gaffer.WeakMethod( self.__pathSelected ) )

	def disconnect( self ) :

		GafferUI.BrowserEditor.Mode.disconnect( self )

		self.__pathSelectedConnection = None

	def _initialPath( self ) :

		return GafferCortex.ClassLoaderPath( self.__classLoader, "/" )

	def _initialDisplayMode( self ) :

		return GafferUI.PathListingWidget.DisplayMode.Tree

	def _initialColumns( self ) :

		return [ GafferUI.PathListingWidget.defaultNameColumn ]

	def __pathSelected( self, pathListing ) :

		selectedPaths = pathListing.getSelectedPaths()
		if not len( selectedPaths ) :
			return

		op = selectedPaths[0].classLoader().load( str( selectedPaths[0] )[1:] )()
		node = GafferCortex.ParameterisedHolderNode()
		node.setParameterised( op )
		GafferCortexUI.ParameterPresets.autoLoad( node )

		opDialogue = GafferCortexUI.OpDialogue( node, executeInBackground = True )
		pathListing.ancestor( GafferUI.Window ).addChildWindow( opDialogue )
		opDialogue.setVisible( True )

GafferUI.BrowserEditor.registerMode( "Ops", OpMode )
GafferUI.BrowserEditor.OpMode = OpMode