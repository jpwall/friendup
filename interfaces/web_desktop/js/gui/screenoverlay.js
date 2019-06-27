/*©agpl*************************************************************************
*                                                                              *
* This file is part of FRIEND UNIFYING PLATFORM.                               *
* Copyright (c) Friend Software Labs AS. All rights reserved.                  *
*                                                                              *
* Licensed under the Source EULA. Please refer to the copy of the GNU Affero   *
* General Public License, found in the file license_agpl.txt.                  *
*                                                                              *
*****************************************************************************©*/

// This is an object!
var ScreenOverlay = {
	visibility: false,
	mode: false,
	list: [],
	// Public methods ----------------------------------------------------------
	init: function()
	{
		this.div = document.createElement( 'div' );
		this.div.id = 'FriendScreenOverlay';
		document.body.appendChild( this.div );
	},
	// Show self
	show: function()
	{
		var self = this;
		if( this.visibility || !this.div ) return;
		this.visibility = true;
		this.div.classList.remove( 'Hidden' );
		this.div.classList.add( 'Visible' );
		setTimeout( function()
		{
			self.div.classList.add( 'Showing' );
		}, 5 );
	},
	// Hide self
	hide: function()
	{
		var self = this;
		if( !this.visibility ) return;
		this.div.classList.add( 'Hiding' );
		setTimeout( function()
		{
			self.div.classList.remove( 'Showing' );
			self.div.classList.remove( 'Hiding' );
			setTimeout( function()
			{
				self.div.classList.add( 'Hidden' );
				self.div.classList.remove( 'Visible' );
				self.visibility = false; // Done hiding!
				self.clearContent();
			}, 250 );
		}, 250 );
	},
	setMode: function( mode )
	{
		switch( mode )
		{
			case 'text':
			case 'list':
				this.mode = mode;
				break;
			default:
				return false;
		}
	},
	setTitle: function( tt )
	{
		if( !this.div.stitle )
		{
			// Add box title
			var title = document.createElement( 'div' );
			title.className = 'Title';
			this.div.appendChild( title );
			this.div.stitle = title;
		}
		this.div.stitle.innerHTML = tt;
		
		var self = this;
		setTimeout( function()
		{
			self.div.stitle.classList.add( 'Showing' );
		}, 5 );
	},
	addStatus: function( topic, content )
	{
		if( !this.div.status )
		{
			// Add box status
			var status = document.createElement( 'div' );
			status.className = 'StatusBox';
			this.div.appendChild( status );
			this.div.status = status;
		}
		this.list.push( { topic: topic, content: content, status: 'Pending' } );
		this.renderStatus();
		return this.list.length - 1;
	},
	editStatus: function( slot, status )
	{
		this.list[ slot ].status = status;
		this.renderStatus();
	},
	renderStatus: function()
	{
		var str = '';
		for( var a = 0; a < this.list.length; a++ )
		{
			str += '<div class="HRow">';
			str += '<div class="HContent15 Topic FloatLeft">' + this.list[a].topic + ':</div>';
			str += '<div class="HContent45 Content FloatLeft">' + this.list[a].content + '</div>';
			str += '<div class="HContent40 Status FloatLeft ' + this.list[a].status + '">' + i18n( 'i18n_status_' + this.list[a].status ) + '</div>';
			str += '</div>';
		}
		str += '';
		this.div.status.innerHTML = str;
	},
	clearContent: function()
	{
		this.list = [];
		this.div.status.innerHTML = '';
	}
};

