/*©mit**************************************************************************
*                                                                              *
* This file is part of FRIEND UNIFYING PLATFORM.                               *
* Copyright (c) Friend Software Labs AS. All rights reserved.                  *
*                                                                              *
* Licensed under the Source EULA. Please refer to the copy of the MIT License, *
* found in the file license_mit.txt.                                           *
*                                                                              *
*****************************************************************************©*/

#include "mobile_app.h"
#include "mobile_app_websocket.h"
#include <pthread.h>
#include <util/hashmap.h>
#include <system/json/jsmn.h>
#include <system/user/user.h>
#include <system/systembase.h>
#include <time.h>
#include <unistd.h>
#include <util/log/log.h>
#include <util/session_id.h>
#include <system/notification/notification.h>
#include <util/sha256.h>
#include <iconv.h>

#define KEEPALIVE_TIME_s 180 //ping time (10 before)
#define ENABLE_MOBILE_APP_NOTIFICATION_TEST_SIGNAL 1

#define CKPT DEBUG("====== %d\n", __LINE__)

#if ENABLE_MOBILE_APP_NOTIFICATION_TEST_SIGNAL == 1
#include <signal.h>
void MobileAppTestSignalHandler(int signum);
#endif

#define WEBSOCKET_SEND_QUEUE

typedef struct UCEntry
{
	char *u;
	UserMobileAppConnections *c;
	MinNode node;
}UCEntry;

//static Hashmap *globalUserToAppConnectionsMap = NULL;

UCEntry *globalUserToAppConnection;

int CAddToList( char *usernname, UserMobileAppConnections *lc )
{
	UCEntry *e = globalUserToAppConnection;
	while( e != NULL )
	{
		if( e->u != NULL && ( strcmp( usernname, e->u ) == 0 ) )
		{
			e->c = lc;
			return 0;
		}
		e = (UCEntry*) e->node.mln_Succ;
	}
	e = FCalloc( 1, sizeof(UCEntry) );
	e->u = usernname;
	e->c = lc;
	e->node.mln_Succ = (MinNode *)globalUserToAppConnection;
	globalUserToAppConnection = e;
	return 0;
}

UserMobileAppConnections *CGetDataFromList( char *username )
{
	UCEntry *e = globalUserToAppConnection;
	while( e != NULL )
	{
		if( e->u != NULL && ( strcmp( username, e->u ) == 0 ) )
		{
			return e->c;
		}
		e = (UCEntry*) e->node.mln_Succ;
	}
	return NULL;
}

static pthread_mutex_t globalSessionRemovalMutex; //used to avoid sending pings while a session is being removed
static pthread_t globalPingThread;

static void  MobileAppInit(void);
static int   MobileAppReplyError( struct lws *wsi, void *udata, int error_code );
static int   MobileAppHandleLogin( struct lws *wsi, void *udata, json_t *json );
static void* MobileAppPingThread( void *a );

/**
 * Write message to websocket
 *
 * @param mac pointer to mobile connection structure
 * @param msg pointer to string where message is stored
 * @param len length of message
 * @return number of bytes written to websocket
 */
static inline int WriteMessageMA( MobileAppConnection *mac, unsigned char *msg, int len )
{
	//MobileAppNotif *man = (MobileAppNotif *) mac->user_data;
	if( mac->mac_WebsocketPtr != NULL )
	{
		if( FRIEND_MUTEX_LOCK( &mac->mac_Mutex ) == 0 )
		{
			FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
			if( en != NULL )
			{
				DEBUG("Message added to queue: '%s' WS pointer: %p\n", msg, mac->mac_WebsocketPtr );
				en->fq_Data = FMalloc( len+32+LWS_SEND_BUFFER_PRE_PADDING+LWS_SEND_BUFFER_POST_PADDING );
				memcpy( en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, msg, len );
				en->fq_Size = LWS_PRE+len;
			
				if( mac->mac_WebsocketPtr != NULL )
				{
					mac->mac_Used++;
					FQPushFIFO( &(mac->mac_Queue), en );
				
					mac->mac_Used--;
				}

				if( mac->mac_WebsocketPtr != NULL )
				{
					lws_callback_on_writable( mac->mac_WebsocketPtr );
				}
			}
			FRIEND_MUTEX_UNLOCK( &(mac->mac_Mutex) );
		}
	}
	return len;
}

/**
 * Initialize Mobile App
 */
static void MobileAppInit( void )
{
	DEBUG("Initializing mobile app module\n");
	
	globalUserToAppConnection = NULL;
	/*
	if( globalUserToAppConnectionsMap == NULL )
	{
		globalUserToAppConnectionsMap = HashmapNew();
	}
	*/

	pthread_mutex_init( &globalSessionRemovalMutex, NULL );
}


/**
 * Remove Connection from global list
 *
 * @param connections pointer to global list with connections
 * @param connectionIndex number of entry which will be removed from list
 */
static void MobileAppRemoveAppConnection( UserMobileAppConnections *connections, int connectionIndex )
{
	if( connectionIndex < 0 && connectionIndex >= MAX_CONNECTIONS_PER_USER )
	{
		FERROR("Connection index is bad: %d!\n", connectionIndex );
		return;
	}
	if( connections == NULL || connections->umac_Connection[connectionIndex] == NULL )
	{
		FERROR("Connection is equal to NULL index %d\n", connectionIndex );
		return;
	}

	// do not remove entry if its in usage
	while( connections->umac_InUse != 0 )
	{
		usleep( 1000 );
	}
	
	DEBUG("\t\t\t\t\t\t\t\t\t\t\tWEBSOCKETS REMOVED FROM LIST : Websocketpointer: %p Position: %d\n", connections->umac_Connection[connectionIndex]->mac_WebsocketPtr, connectionIndex );
	
	MobileAppConnection *oldConnection = connections->umac_Connection[connectionIndex];
	if( oldConnection != NULL )
	{
		oldConnection->mac_UserConnections = NULL;
	}
	connections->umac_Connection[connectionIndex]->mac_WebsocketPtr = NULL;
	//lws_callback_on_writable( connections->umac_Connection[connectionIndex]->mac_WebsocketPtr );
	connections->umac_Connection[connectionIndex] = NULL;
}

/**
 * Add user connectiono to global list
 *
 * @param con pointer to new connection structure
 * @param username name of user which passed login
 * @param userData pointer to user data
 * @return 0 when success, otherwise error number
 */
int MobileAppAddNewUserConnection( MobileAppConnection *con, const char *username, void *userData )
{
	FBOOL createNewConnection = FALSE;
	if( username == NULL )
	{
		FERROR("Username is equal to NULL!\n");
		return 1;
	}
	UserMobileAppConnections *userConnections = NULL;
	if( FRIEND_MUTEX_LOCK( &globalSessionRemovalMutex ) == 0 )
	{
		//userConnections = GetConnectionsByUserName( globalUserToAppConnections, (char *)username );
		DEBUG("Hashmap get 2 : %s\n", username );
		//userConnections = HashmapGetData( globalUserToAppConnectionsMap, username);
		userConnections = CGetDataFromList( (char *)username );
		FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
	}
	DEBUG("[MobileAppAddNewUserConnection] existing userConnections: %p\n", userConnections );

	if( userConnections == NULL )
	{ //this user does not have any connections yet
		//create a new connections struct
		userConnections = FCalloc(sizeof( UserMobileAppConnections ), 1);
		DEBUG("[MobileAppAddNewUserConnection] allocate new user connection\n");

		if( userConnections == NULL )
		{
			DEBUG("Allocation failed\n");
			return 2;//MobileAppReplyError(wsi, user_data, MOBILE_APP_ERR_INTERNAL);
		}
		else
		{
			DEBUG("Creating new struct for user <%s>\n", username);
			char *permanentUsername = StringDuplicate( username );
			userConnections->umac_Username = permanentUsername;
			createNewConnection = TRUE;
			
			//
			// we must also attach UserID to User. This functionality will allow FC to find user by ID
			//
			
			SQLLibrary *sqllib  = SLIB->LibrarySQLGet( SLIB );

			userConnections->umac_UserID = 0;
			if( sqllib != NULL )
			{
				char *qery = FMalloc( 1048 );
				qery[ 1024 ] = 0;
				sqllib->SNPrintF( sqllib, qery, 1024, "SELECT ID FROM FUser WHERE `Name`=\"%s\"", username );
				void *res = sqllib->Query( sqllib, qery );
				if( res != NULL )
				{
					char **row;
					if( ( row = sqllib->FetchRow( sqllib, res ) ) )
					{
						if( row[ 0 ] != NULL )
						{
							char *end;
							userConnections->umac_UserID = strtoul( row[0], &end, 0 );
						}
					}
					sqllib->FreeResult( sqllib, res );
				}
				SLIB->LibrarySQLDrop( SLIB, sqllib );
				FFree( qery );
			}

			//FIXME: check the deallocation order for permanent_username as it is held both
			//by our internal sturcts and within hashmap structs

			DEBUG("Will add entry to hashmap!\n");
			//PutConnectionsByUserName( globalUserToAppConnections, permanentUsername, userConnections );
			if( FRIEND_MUTEX_LOCK( &globalSessionRemovalMutex ) == 0 )
			{
				// add the new connections struct to global users' connections map
				DEBUG("Hashmap put: %s\n", permanentUsername );
				CAddToList( permanentUsername, userConnections );
				/*
				if( HashmapPut( globalUserToAppConnectionsMap, permanentUsername, userConnections ) != MAP_OK )
				{
					DEBUG("Could not add new struct of user <%s> to global map\n", username);

					FFree(userConnections);
					FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
					return 3;
				}
				*/
				
				FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
			}
			createNewConnection = TRUE;	// there is new entry for user, we must create connection
		}
	}
	else
	{
		DEBUG("User connections is not null!\n");
	}
	
	if( createNewConnection == TRUE )
	{
		userConnections->umac_Connection[ 0 ] = con;

		con->mac_UserData = userData;
		con->mac_UserConnections = userConnections; //provide back reference that will map websocket to a user
		con->mac_UserConnectionIndex = 0;
	}
	else
	{
		//add this struct to user connections struct
		int connectionToReplaceIndex = -1;
		for( int i = 0; i < MAX_CONNECTIONS_PER_USER; i++ )
		{
			FBOOL sameUMA = FALSE;
			// if there is free place or we have same WS connection
		
			if( userConnections->umac_Connection[i] != NULL && con->mac_UserMobileAppID > 0 && userConnections->umac_Connection[i]->mac_UserMobileAppID > 0 )
			{
				if( con->mac_UserMobileAppID == userConnections->umac_Connection[i]->mac_UserMobileAppID )
				{
					DEBUG("[MobileAppAddNewUserConnection] seems two connections have same UMAID!\n");
					sameUMA = TRUE;
				}
			}
		
			if( userConnections->umac_Connection[i] == NULL || userConnections->umac_Connection[i]->mac_WebsocketPtr == con->mac_WebsocketPtr || sameUMA == TRUE )
			{ //got empty slot
			
				connectionToReplaceIndex = i;
				DEBUG("Will use slot %d for this connection, sameUMA? %d connection pointer %p\n", connectionToReplaceIndex, sameUMA, userConnections->umac_Connection[i] );
				break;
			}
		}

		if( connectionToReplaceIndex == -1 )
		{ //no empty slots found - drop the oldest connection

			connectionToReplaceIndex = 0;
			unsigned int oldest_timestamp = userConnections->umac_Connection[0]->mac_LastCommunicationTimestamp;

			for( int i = 1; i < MAX_CONNECTIONS_PER_USER; i++ )
			{
				if( userConnections->umac_Connection[i] != NULL )
				{
					if( userConnections->umac_Connection[i]->mac_LastCommunicationTimestamp < oldest_timestamp )
					{
						oldest_timestamp = userConnections->umac_Connection[i]->mac_LastCommunicationTimestamp;
						connectionToReplaceIndex = i;
						DEBUG("Will drop old connection from slot %d (last comm %d)\n", connectionToReplaceIndex, oldest_timestamp);
					}
				}
			}
		}

		DEBUG("Create new connection or update old one? index: %d pointer %p\n", connectionToReplaceIndex, userConnections->umac_Connection[connectionToReplaceIndex] );
		if( userConnections->umac_Connection[connectionToReplaceIndex] != NULL )
		{
			//MobileAppRemoveAppConnection( userConnections, connectionToReplaceIndex );
		
			//newConnection = userConnections->umac_Connection[connectionToReplaceIndex];
			con->mac_UserData = userData;
			con->mac_UserConnections = userConnections; //provide back reference that will map websocket to a user
			con->mac_UserConnectionIndex = connectionToReplaceIndex;
		}
		else
		{
			userConnections->umac_Connection[ connectionToReplaceIndex ] = con;

			con->mac_UserData = userData;
			con->mac_UserConnections = userConnections; //provide back reference that will map websocket to a user
			con->mac_UserConnectionIndex = connectionToReplaceIndex;
		}
	}

	DEBUG("\t\t\tAdded\n");
	
	return 0;
}

/**
 * Websocket callback delivered from a mobile app. This function is called from within websocket.c
 * when a frame is received.
 *
 * @param wsi pointer to main Websockets structure
 * @param reason type of received message (lws_callback_reasons type)
 * @param user user data
 * @param in message (array of chars)
 * @param len size of 'message'
 * @return 0 when success, otherwise error number
 */
int WebsocketAppCallback(struct lws *wsi, int reason, void *user __attribute__((unused)), void *in, size_t len )
{
	//DEBUG("websocket callback, reason %d, len %zu, wsi %p\n", reason, len, wsi);
	MobileAppNotif *man = (MobileAppNotif *) user;
	
	if( reason == LWS_CALLBACK_PROTOCOL_INIT )
	{
		MobileAppInit();
		return 0;
	}
	
	DEBUG1("-------------------\nwebsocket_app_callback\n------------------\nreasond: %d\n", reason );

	MobileAppConnection *appConnection = NULL;
		
	if( FRIEND_MUTEX_LOCK( &globalSessionRemovalMutex ) == 0 )
	{
		//appConnection = GetConnectionByWSI( globalWebsocketToUserConnections, wsi );
		if( user != NULL )
		{
			MobileAppNotif *n = (MobileAppNotif *)user;
			appConnection = n->man_Data;
		}
		FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
	}
	DEBUG("Checking: connection to app pointer %p\n", appConnection );
	
	switch( reason )
	{
		case LWS_CALLBACK_ESTABLISHED:
			{
				MobileAppNotif *n = (MobileAppNotif *)user;
				n->man_Data = MobileAppConnectionNew( wsi, 0, NULL );
			}
			break;
			
		case LWS_CALLBACK_CLOSED: //|| reason == LWS_CALLBACK_WS_PEER_INITIATED_CLOSE)
		{
			Log( FLOG_DEBUG, "Closing connection: appConnection %p\n", appConnection );
			
			if( appConnection == NULL )
			{
				DEBUG("Websocket close - no user session found for this socket\n");
				return 0;
				//return MobileAppReplyError( wsi, user, MOBILE_APP_ERR_NO_SESSION_NO_CONNECTION );
			}
			Log( FLOG_DEBUG, "\t\t\t\t\t\t\tREMOVE APP CONNECTION %d - conptr %p\n", appConnection->mac_UserConnectionIndex, appConnection );
			
			if( FRIEND_MUTEX_LOCK( &globalSessionRemovalMutex ) == 0 )
			{
				
				//remove connection from user connnection struct
				UserMobileAppConnections *userConnections = appConnection->mac_UserConnections;
				unsigned int connectionIndex = appConnection->mac_UserConnectionIndex;
				if( userConnections != NULL )
				{
					DEBUG("Removing connection %d for user <%s>\n", connectionIndex, userConnections->umac_Username );
					MobileAppRemoveAppConnection( userConnections, connectionIndex );
				}
			
				FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
			}
			
			// do not close connection if its used
			while( TRUE )
			{
				if( appConnection->mac_Used <= 0 )
				{
					break;
				}
				sleep( 1 );
			}

			MobileAppConnectionDelete( appConnection );
			//TODO

				//FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
			//}
			MobileAppNotif *n = (MobileAppNotif *)user;
			n->man_Data = NULL;
			//FFree( websocketHash );
			//return 0;
		}
		break;
		
		case LWS_CALLBACK_SERVER_WRITEABLE:
		{
#ifdef WEBSOCKET_SEND_QUEUE
			FQEntry *e = NULL;
			
			if( appConnection == NULL )
			{
				FERROR("Appconnection is NULL!\n");
				return 0;
			}
			if( appConnection->mac_Queue.fq_First == NULL )
			{
				FERROR("We cannot send message without data\n");
				return 0;
			}
			
			if( appConnection->mac_WebsocketPtr == NULL )
			{
				FERROR("Websocket connection removed, closing down\n");
				return 1;
			}
			
			if( FRIEND_MUTEX_LOCK( &(appConnection->mac_Mutex) ) == 0 )
			{
				MobileAppNotif *n = (MobileAppNotif *)user;
				appConnection = n->man_Data;
			
				//FQueue *q = &(man->man_Queue);
				FQueue *q = &(appConnection->mac_Queue);
			
				DEBUG("[websocket_app_callback] WRITABLE CALLBACK, q %p pointer to WS: %p\n", q, wsi );
			
				e = FQPop( q );
			
				if( e != NULL )
				{
					unsigned char *t = e->fq_Data+LWS_SEND_BUFFER_PRE_PADDING;
					t[ e->fq_Size+1 ] = 0;

					int res = lws_write( wsi, t, e->fq_Size, LWS_WRITE_TEXT );
					DEBUG("[websocket_app_callback] message sent: %s len %d\n", (char *)t, res );

					int ret = lws_send_pipe_choked( wsi );
				
					if( e != NULL )
					{
						DEBUG("Release: %p\n", e->fq_Data );
						if( e->fq_Data != NULL )
						{
							FFree( e->fq_Data );
							e->fq_Data = NULL;
						}
						FFree( e );
					}
				}
				else
				{
					DEBUG("[websocket_app_callback] No message in queue\n");
				}
			
				// test
				FRIEND_MUTEX_UNLOCK( &appConnection->mac_Mutex );
			}
			
			if( appConnection != NULL && appConnection->mac_Queue.fq_First != NULL )
			{
				DEBUG("We have message to send, calling writable\n");
				lws_callback_on_writable( wsi );
			}
			DEBUG("writable end\n" );
#endif
			return 0;
		}
		break;
		
		case LWS_CALLBACK_RECEIVE:
		{
			char *data = (char*)in;

			if( len == 0 )
			{
				DEBUG("Empty websocket frame (reason %d)\n", reason);
				return 0;
			}

			data[ len ] = 0;
			//DEBUG("Mobile app data: <%*s>, len: %d\n", (unsigned int)len, data, len );
			DEBUG("Mobile app data: <%s>, len: %lu\n", data, len );

			jsmn_parser parser;
			jsmn_init(&parser);
			jsmntok_t tokens[16]; //should be enough

			int tokensFound = jsmn_parse(&parser, data, len, tokens, sizeof(tokens)/sizeof(tokens[0]));

			DEBUG("JSON tokens found %d\n", tokensFound);

			if( tokensFound < 1 )
			{
				FERROR("Bad message\n");
				return MOBILE_APP_ERR_NO_JSON;
				//MobileAppReplyError( wsi, user, MOBILE_APP_ERR_NO_JSON );
			}

			json_t json = { .string = data, .string_length = len, .token_count = tokensFound, .tokens = tokens };

			char *msgTypeString = json_get_element_string(&json, "t");

			if( msgTypeString )
			{
				//due to uniqueness of "t" field values only first letter has to be evaluated
				char firstTypeLetter = msgTypeString[0];
				DEBUG("Type letter <%c>\n", firstTypeLetter);

				if( firstTypeLetter == 'l'/*login*/)
				{
					int ret = MobileAppHandleLogin( wsi, user, &json );
					
					Log( FLOG_DEBUG, "ADD APP CONNECTION Websocket pointer: %p login return error: %d\n", wsi, ret );

					char response[64+LWS_PRE];
					int som = snprintf(response+LWS_PRE, sizeof(response), "{ \"t\":\"login\", \"status\":%d}", ret );
					lws_write(wsi, (unsigned char*)response+LWS_PRE, strlen(response+LWS_PRE), LWS_WRITE_TEXT);	// bad hack
					return ret;		// remove WS connection if login fail
				}
				else
				{
					Log( FLOG_INFO, "MobileAppConnection: %p message received: %s\n", appConnection, data );
					
					if( appConnection == NULL)
					{
						DEBUG("Session not found for this connection\n");
						char response[64+LWS_PRE];
						int som = snprintf(response+LWS_PRE, sizeof(response), "{ \"t\":\"error\", \"status\":%d}", MOBILE_APP_ERR_NO_SESSION );
						lws_write(wsi, (unsigned char*)response+LWS_PRE, strlen(response+LWS_PRE), LWS_WRITE_TEXT);	// bad hack
						//WriteMessageMA( appConnection, (unsigned char*)response, som );
						DEBUG("APP connection is equal to NULL!\n");
						return MOBILE_APP_ERR_NO_SESSION;
					}

					appConnection->mac_LastCommunicationTimestamp = time(NULL);

					switch (firstTypeLetter)
					{
						case 'p': 
							do
							{ //pause
								DEBUG("App is paused\n");
								appConnection->mac_AppStatus = MOBILE_APP_STATUS_PAUSED;
								appConnection->mac_MostRecentPauseTimestamp = time(NULL);
					
								char response[LWS_PRE+64];
								strcpy(response+LWS_PRE, "{\"t\":\"pause\",\"status\":1}");
								DEBUG("Response: %s\n", response+LWS_PRE);
					
#ifndef WEBSOCKET_SINK_SEND_QUEUE
								FRIEND_MUTEX_LOCK(&globalSessionRemovalMutex);
								lws_write(wsi, (unsigned char*)response+LWS_PRE, strlen(response+LWS_PRE), LWS_WRITE_TEXT);
								FRIEND_MUTEX_UNLOCK(&globalSessionRemovalMutex);
#else
								FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
								if( en != NULL )
								{
									en->fq_Data = FMalloc( 64+LWS_SEND_BUFFER_PRE_PADDING+LWS_SEND_BUFFER_POST_PADDING );
									memcpy( en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, "{\"t\":\"pause\",\"status\":1}", 24 );
									en->fq_Size = LWS_PRE+64;
						
									DEBUG("[websocket_app_callback] Msg to send: %s\n", en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING );
			
									if( FRIEND_MUTEX_LOCK( &(appConnection->mac_Mutex) ) == 0 )
									{
										FQPushFIFO( &(appConnection->mac_Queue), en );
										FRIEND_MUTEX_UNLOCK( &(appConnection->mac_Mutex) );
									}
									lws_callback_on_writable( wsi );
								}
#endif
							}
							while (0);
						break;

						case 'r': 
							do
							{ //resume
								DEBUG("App is resumed\n");
								appConnection->mac_AppStatus = MOBILE_APP_STATUS_RESUMED;
								appConnection->mac_MostRecentResumeTimestamp = time(NULL);
					
								char response[LWS_PRE+64];
								strcpy(response+LWS_PRE, "{\"t\":\"resume\",\"status\":1}");
								DEBUG("Response: %s\n", response+LWS_PRE);
#ifndef WEBSOCKET_SINK_SEND_QUEUE
								FRIEND_MUTEX_LOCK(&globalSessionRemovalMutex);
								lws_write(wsi, (unsigned char*)response+LWS_PRE, strlen(response+LWS_PRE), LWS_WRITE_TEXT);
								FRIEND_MUTEX_UNLOCK(&globalSessionRemovalMutex);
#else
								FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
								if( en != NULL )
								{
									en->fq_Data = FMalloc( 64+LWS_SEND_BUFFER_PRE_PADDING+LWS_SEND_BUFFER_POST_PADDING );
									memcpy( en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, "{\"t\":\"resume\",\"status\":1}", 25 );
									en->fq_Size = LWS_PRE+64;
						
									DEBUG("[websocket_app_callback] Msg to send1: %s\n", en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING );
			
									if( FRIEND_MUTEX_LOCK( &(appConnection->mac_Mutex) ) == 0 )
									{
										FQPushFIFO( &(appConnection->mac_Queue), en );
										FRIEND_MUTEX_UNLOCK( &(appConnection->mac_Mutex) );
									}
									lws_callback_on_writable( wsi );
								}
#endif
							}
							while (0);
						break;
			
						// get information from mobile device about notification status
						/*
						JSONObject notifyReply = new JSONObject();
							notifyReply.put("t", "notify");
							notifyReply.put("status", "received");
							notifyReply.put("id", id);
						*/
						case 'n':
							{
								char *statusString = json_get_element_string( &json, "status" );
								char *idString = json_get_element_string( &json, "id" );
					
								DEBUG("Notification information received. Status: %s id: %s\n", statusString, idString );
					
								if( statusString != NULL && idString != NULL )
								{
									DEBUG("NotificationSent will change status: %s\n", statusString );
						
									FULONG id = 0;
									int status = 0;
									char *end;
						
									id = strtol( idString, &end, 0 );
									status = atoi( statusString );
						
									NotificationManagerNotificationSentSetStatusDB( SLIB->sl_NotificationManager, id, status );
								}
							}
						break;

						// get ping and response via pong
						/*
						JSONObject pingRequest = new JSONObject();
							pingRequest.put("t", "echo");
							pingRequest.put("time", System.currentTimeMillis());
						*/
						case 'e': //echo
							{
								char *timeString = json_get_element_string( &json, "time" );
					
								char response[LWS_PRE+64];
								snprintf( response+LWS_PRE, 64, "{\"t\":\"pong\",\"time\":\"%s\"}", timeString );
								DEBUG("Response: %s\n", response+LWS_PRE);
#ifndef WEBSOCKET_SINK_SEND_QUEUE
								FRIEND_MUTEX_LOCK(&globalSessionRemovalMutex);
								lws_write(wsi, (unsigned char*)response+LWS_PRE, strlen(response+LWS_PRE), LWS_WRITE_TEXT);
								FRIEND_MUTEX_UNLOCK(&globalSessionRemovalMutex);
#else
								FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
								if( en != NULL )
								{
									en->fq_Data = FMalloc( 64+LWS_SEND_BUFFER_PRE_PADDING+LWS_SEND_BUFFER_POST_PADDING );
									int msgsize = snprintf( (char *)(en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING), 64, "{\"t\":\"pong\",\"time\":\"%s\"}", timeString );
									en->fq_Size = msgsize;
									
									UserSession *us = appConnection->mac_UserSession;
									if( us != NULL )
									{
										us->us_LoggedTime = time( NULL );
									}
						
									DEBUG("[websocket_app_callback] Msg to send1: %s pointer to user session %p\n", en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, us );
			
									if( FRIEND_MUTEX_LOCK( &(appConnection->mac_Mutex) ) == 0 )
									{
										FQPushFIFO( &(appConnection->mac_Queue), en );
										FRIEND_MUTEX_UNLOCK( &(appConnection->mac_Mutex) );
									}
									lws_callback_on_writable( wsi );
								}
#endif
							}
						break;
						
						case 'g': //get old notification
							{
								char *get = json_get_element_string( &json, "get" );
								
								FULONG umaID = appConnection->mac_UserMobileAppID;
								if( umaID > 0 )
								{
									// get all NotificationSent structures with register state and which belongs to this user mobile application (UserMobileAppID)
									NotificationSent *nsroot = NotificationManagerGetNotificationsSentByStatusPlatformAndUMAIDDB( SLIB->sl_NotificationManager, NOTIFICATION_SENT_STATUS_REGISTERED, MOBILE_APP_TYPE_ANDROID, umaID );
									DEBUG("NotificationSent ptr %p\n", nsroot );
									NotificationSent *ns = nsroot;
									while( ns != NULL )
									{
										DEBUG("Going through messages: %lu\n", ns->ns_ID );
										Notification *notif = NotificationManagerGetDB( SLIB->sl_NotificationManager, ns->ns_NotificationID );
										int reqLengith = 512;
										// send notification to device
										DEBUG("Notification pointer %p\n", notif );
										if( notif != NULL )//&& ((notif->n_Created+TIME_OF_OLDER_MESSAGES_TO_REMOVE) < time(NULL)) )
										{
											if( notif->n_Channel != NULL )
											{
												reqLengith += strlen( notif->n_Channel );
											}
		
											if( notif->n_Content != NULL )
											{
												reqLengith += strlen( notif->n_Content );
											}
		
											if( notif->n_Title != NULL )
											{
												reqLengith += strlen( notif->n_Title );
											}
		
											if( notif->n_Application != NULL )
											{
												reqLengith += strlen( notif->n_Application );
											}
		
											if( notif->n_Extra != NULL )
											{
												reqLengith += strlen( notif->n_Extra );
											}
					
											char *jsonMessage = FMalloc( reqLengith );
											if( jsonMessage != NULL )
											{
												unsigned int jsonMessageLength = 0;
#ifdef WEBSOCKET_SEND_QUEUE
												if( notif->n_Extra )
												{ //TK-1039
													jsonMessageLength = snprintf( jsonMessage, reqLengith, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"%s\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Extra, notif->n_Application, ns->ns_ID, notif->n_ID );
												}
												else
												{
													jsonMessageLength = snprintf( jsonMessage, reqLengith, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Application, ns->ns_ID, notif->n_ID );
												}
						
												DEBUG("Message will be sent through websockets\n");
						
												WriteMessageMA( appConnection, (unsigned char*)jsonMessage, jsonMessageLength );
#else
												if( notif->n_Extra )
												{ //TK-1039
													jsonMessageLength = snprintf( jsonMessage + LWS_PRE, reqLengith-LWS_PRE, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"%s\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Extra, notif->n_Application, ns->ns_ID, notif->n_ID );
												}
												else
												{
													jsonMessageLength = snprintf( jsonMessage + LWS_PRE, reqLengith-LWS_PRE, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Application, ns->ns_ID, notif->n_ID );
												}
						
												if( userConnections->umac_Connection[i] != NULL && userConnections->umac_Connection[i]->mac_WebsocketPtr != NULL )
												{
													lws_write( userConnections->umac_Connection[i]->mac_WebsocketPtr,(unsigned char*)jsonMessage+LWS_PRE,jsonMessageLength,LWS_WRITE_TEXT);
												}
#endif

												FFree( jsonMessage );
											}
											
										}
										else	// notification was received (it doesnt exist in database), we can remove entry
										{
											NotificationManagerDeleteNotificationSentDB( SLIB->sl_NotificationManager, ns->ns_ID );
										}
										if( notif != NULL )
										{
											NotificationDelete( notif );
											notif = NULL;
										}
										
										// maybe it should be confirmed by app?
										//NotificationManagerNotificationSentSetStatusDB( SLIB->sl_NotificationManager, ns->ns_ID, NOTIFICATION_SENT_STATUS_RECEIVED );

										ns = (NotificationSent *)ns->node.mln_Succ;
									}
									NotificationSentDeleteAll( nsroot );
								}
							}
						break;
						
						case LWS_CALLBACK_PROTOCOL_DESTROY:
						{
							UCEntry *e = globalUserToAppConnection;
							while( e != NULL )
							{
								UCEntry *de = e;
								e = (UCEntry *)e->node.mln_Succ;
								
								if( de->u != NULL )
								{
									FFree( de->u );
									FFree( de );
								}
							}
						}
						break;

						default:
							return 0;//MobileAppReplyError(wsi, user, MOBILE_APP_ERR_WRONG_TYPE);
					}
				}
			}
			else
			{
				return 0;//MobileAppReplyError(wsi, user, MOBILE_APP_ERR_NO_TYPE);
			}
		}
		break;
	}
	return 0; //should be unreachable
}

/**
 * Sends an error reply back to the app and closes the websocket.
 *
 * @param wsi pointer to a Websockets struct
 * @param error_code numerical value of the error code
 */
static int MobileAppReplyError(struct lws *wsi, void *user, int error_code)
{
	if( error_code == MOBILE_APP_ERR_NO_SESSION_NO_CONNECTION )
	{
		
	}
	else
	{
		char response[LWS_PRE+32];
		snprintf(response+LWS_PRE, sizeof(response)-LWS_PRE, "{ \"t\":\"error\", \"status\":%d}", error_code);
		DEBUG("Error response: %s\n", response+LWS_PRE);

#ifdef WEBSOCKET_SEND_QUEUE
		//WriteMessageMA( user_connections->connection[i], (unsigned char*)jsonMessage, msgSendLength );
#else
		DEBUG("WSI %p\n", wsi);
		lws_write(wsi, (unsigned char*)response+LWS_PRE, strlen(response+LWS_PRE), LWS_WRITE_TEXT);
#endif
	}

	/*
	MobileAppConnection *appConnection = NULL;
	if( FRIEND_MUTEX_LOCK( &globalSessionRemovalMutex ) == 0 )
	{
		MobileAppNotif *n = (MobileAppNotif *)user;
		appConnection = n->man_Data;

		FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
	}
	
	//FFree(websocketHash);
	if( appConnection )
	{
		DEBUG("Cleaning up before closing socket\n");
		UserMobileAppConnections *userConnections = appConnection->mac_UserConnections;
		unsigned int connectionIndex = appConnection->mac_UserConnectionIndex;
		if( userConnections != NULL )
		{
			DEBUG("Removing connection %d for user <%s>\n", connectionIndex, userConnections->umac_Username);
		}
		MobileAppRemoveAppConnection( userConnections, connectionIndex );
	}*/

	return -1;
}

/**
 * User login handler
 *
 * @param wsi pointer to Websocket structure
 * @param json to json structure with login entry
 * @return 0 when success, otherwise error number
 */
static int MobileAppHandleLogin( struct lws *wsi, void *userdata, json_t *json )
{
	char *usernameString = json_get_element_string( json, "user" );

	if( usernameString == NULL )
	{
		return MOBILE_APP_ERR_LOGIN_NO_USERNAME;//MobileAppReplyError(wsi, userdata, MOBILE_APP_ERR_LOGIN_NO_USERNAME);
	}

	char *passwordString = json_get_element_string(json, "pass");

	if( passwordString == NULL )
	{
		return MOBILE_APP_ERR_LOGIN_NO_PASSWORD;//MobileAppReplyError(wsi, userdata, MOBILE_APP_ERR_LOGIN_NO_PASSWORD);
	}
	
	char *sessionid = json_get_element_string( json, "sessionid" );
	char *tokenString = json_get_element_string(json, "apptoken");

	//step 3 - check if the username and password is correct
	DEBUG("Login attempt <%s> <%s> tokenString: %s\n", usernameString, passwordString, tokenString );

	unsigned long block_time = 0;
	User *user = NULL;
	FULONG umaID = 0;
	
	SQLLibrary *sqlLib = SLIB->LibrarySQLGet( SLIB );
	if( sqlLib != NULL )
	{
		// wait till User Manager will not be equal to NULL
		if( SLIB->sl_UM == NULL )
		{
			while( TRUE )
			{
				DEBUG("Waiting for UserMobileManager\n");
				if( SLIB->sl_UM != NULL )
				{
					break;
				}
				sleep( 1 );
			}
		}
		
		user = UMGetUserByNameDBCon( SLIB->sl_UM, sqlLib, usernameString );
	
		if( tokenString != NULL && user != NULL )
		{
			umaID = MobileManagerGetUMAIDByTokenAndUserName( SLIB->sl_MobileManager, sqlLib, user->u_ID, tokenString );
		}
		else
		{
			FERROR("TokenApp is NULL!\n");
		}

		SLIB->LibrarySQLDrop( SLIB, sqlLib );
	}
	AuthMod *a = SLIB->AuthModuleGet( SLIB );

	DEBUG("Check password %s \n", passwordString );
	if( a->CheckPassword(a, NULL, user, passwordString, &block_time) == FALSE )
	{
		DEBUG("Check = false\n");
		if( user != NULL )
		{
			UserDelete( user );
			user = NULL;
		}
		return MOBILE_APP_ERR_LOGIN_INVALID_CREDENTIALS;//MobileAppReplyError( wsi, userdata, MOBILE_APP_ERR_LOGIN_INVALID_CREDENTIALS );
	}
	else
	{
		DEBUG("Password is ok\n");
		if( user != NULL )
		{
			UserDelete( user );
			user = NULL;
		}
		
		DEBUG("MobileAppConnectionNew\n");
		
		MobileAppNotif *n = (MobileAppNotif *)userdata;
		MobileAppConnection *con = (MobileAppConnection *)n->man_Data;
		con->mac_UserMobileAppID = umaID;
		con->mac_WebsocketPtr = wsi;
		
		con->mac_UserSession = USMGetSessionBySessionID( SLIB->sl_USM, sessionid );
		
		int err = MobileAppAddNewUserConnection( con, usernameString, userdata );
		
		Log( FLOG_DEBUG, "\t\t\tADD APP CONNECTION Websocket pointer: %p login return error: %p position %d pointer to UserSession %p\n", wsi, con, con->mac_UserConnectionIndex, con->mac_UserSession );
		
		DEBUG("New connection added, umaID: %lu\n", umaID );
		
		return 0;
	}
	return -1;
}

/**
 * Notify user
 *
 * @param lsb pointer to SystemBase
 * @param username pointer to string with user name
 * @param channel_id number of channel
 * @param app application name
 * @param title title of message which will be send to user
 * @param message message which will be send to user
 * @param notification_type type of notification
 * @param extraString additional string which will be send to user
 * @return true when message was send
 */
int MobileAppNotifyUserRegister( void *lsb, const char *username, const char *channel_id, const char *app, const char *title, const char *message, MobileNotificationTypeT notification_type, const char *extraString )
{
	SystemBase *sb = (SystemBase *)lsb;
	UserMobileAppConnections *userConnections = NULL;
	MobileManager *mm = sb->sl_MobileManager;
	Notification *notif = NULL;
	FBOOL wsMessageSent = FALSE;
	
	// get message length
	
	unsigned int reqLengith = 0;
	
	DEBUG("\n\n\n\n---------------------------------------------MobileAppNotifyUserRegister\n");
	
	// if value was not passed, create new Notification
	
	char *escapedChannelId = json_escape_string(channel_id);
	char *escapedTitle = json_escape_string(title);
	char *escapedMessage = json_escape_string(message);
	char *escapedApp = NULL;
	char *escapedExtraString = NULL;
	FULONG userID = 0;
	
	if( app )
	{
		escapedApp = json_escape_string( app );
		reqLengith += strlen( escapedApp );
	}
	if( extraString )
	{
		escapedExtraString = json_escape_string( extraString );
		reqLengith += strlen( escapedExtraString );
	}
	
	notif = NotificationNew();
	if( notif != NULL )
	{
		notif->n_Application = escapedApp;
		notif->n_Channel = escapedChannelId;
		notif->n_UserName = StringDuplicate( username );
		notif->n_Title = escapedTitle;
		notif->n_Content = escapedMessage;
		notif->n_Extra = escapedExtraString;
		notif->n_NotificationType = notification_type;
		notif->n_Status = NOTIFY_ACTION_REGISTER;
		notif->n_Created = time(NULL);
	}
	
	NotificationManagerAddNotificationDB( sb->sl_NotificationManager, notif );
	
	reqLengith += strlen( notif->n_Channel ) + strlen( notif->n_Content ) + strlen( notif->n_Title ) + LWS_PRE + 512/*some slack*/;
	
	// allocate memory for message
	
	char *jsonMessage = FMalloc( reqLengith );
	
	// inform user there is notification for him
	// if there is no connection it means user cannot get message
	// then send him notification via mobile devices
	
	int bytesSent = 0;
	User *usr = UMGetUserByName( sb->sl_UM, username );
	if( usr != NULL )
	{
		userID = usr->u_ID;
		time_t timestamp = time( NULL );
		//
		
		FRIEND_MUTEX_LOCK( &usr->u_Mutex );
		UserSessListEntry  *usl = usr->u_SessionsList;
		while( usl != NULL )
		{
			UserSession *locses = (UserSession *)usl->us;
			if( locses != NULL )
			{
				//WSCData *data = (WSCData *)us_WSConnections;
				DEBUG("[AdminWebRequest] Send Message through websockets: %s clients: %p timestamptrue: %d\n", locses->us_DeviceIdentity, locses->us_WSConnections, ( ( (timestamp - locses->us_LoggedTime) < sb->sl_RemoveSessionsAfterTime ) ) );
				
				if( ( ( (timestamp - locses->us_LoggedTime) < sb->sl_RemoveSessionsAfterTime ) ) && locses->us_WSConnections != NULL )
				{
					int msgLen = 0;
					NotificationSent *lns = NotificationSentNew();
					lns->ns_NotificationID = notif->n_ID;
					lns->ns_RequestID = locses->us_ID;
					lns->ns_Target = MOBILE_APP_TYPE_NONE;	// none means WS
					lns->ns_Status = NOTIFICATION_SENT_STATUS_REGISTERED;
					NotificationManagerAddNotificationSentDB( sb->sl_NotificationManager, lns );
					
					if( notif->n_Extra )
					{ //TK-1039
						msgLen = snprintf( jsonMessage, reqLengith, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"%s\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu, \"source\":\"ws\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Extra, notif->n_Application, lns->ns_ID );
					}
					else
					{
						msgLen = snprintf( jsonMessage, reqLengith, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu, \"source\":\"ws\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Application, lns->ns_ID );
					}
					
					int msgsize = reqLengith + msgLen;
					char *sndbuffer = FMalloc( msgsize );
					
					DEBUG("\t\t\t\t\t\t\t jsonMessage '%s' len %d \n", jsonMessage, reqLengith );
					int lenmsg = snprintf( sndbuffer, msgsize-1, "{\"type\":\"msg\",\"data\":{\"type\":\"notification\",\"data\":{\"id\":\"%lu\",\"notificationData\":%s}}}", lns->ns_ID , jsonMessage );
					
					Log( FLOG_INFO, "Send notification through Websockets: '%s' len %d \n", sndbuffer, msgsize );
					
					bytesSent += WebSocketSendMessageInt( locses, sndbuffer, lenmsg );
					FFree( sndbuffer );
					
					// add NotificationSent to Notification
					lns->node.mln_Succ = (MinNode *)notif->n_NotificationsSent;
					notif->n_NotificationsSent = lns;
				}
			} // locses = NULL
			usl = (UserSessListEntry *)usl->node.mln_Succ;
		}
		//usr = (User *)usr->node.mln_Succ;
		
		FRIEND_MUTEX_UNLOCK( &usr->u_Mutex );
	}	// usr != NULL
	else
	{
		userID = UMGetUserIDByName( sb->sl_UM, username );
	}
	
	if( bytesSent > 0 )
	{
		wsMessageSent = TRUE;
	}
	
	// if message was sent via Websockets
	// then Notification must be added to list, which will be checked before
	if( wsMessageSent == TRUE )
	{
		NotificationManagerAddToList( sb->sl_NotificationManager, notif );
	}
	
	//
	// go through all mobile connections
	//
	
	if( FRIEND_MUTEX_LOCK( &globalSessionRemovalMutex ) == 0 )
	{
		//userConnections = GetConnectionsByUserName( globalUserToAppConnections, (char *)username );
		DEBUG("Hashmap get: %s\n", username );
		userConnections = CGetDataFromList( (char *)username );
		/*
		userConnections = HashmapGetData( globalUserToAppConnectionsMap, username );
		if( userConnections != NULL )
		{
			userConnections->umac_InUse=1;
		}
		*/
		FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
	}
	DEBUG("NotificationRegister: get all connections by name: %s pointer: %p\n", username, userConnections );
	
	BufString *bsMobileReceivedMessage = BufStringNew();
	//unsigned int jsonMessageLength = strlen( jsonMessage + LWS_PRE);
	// if message was already sent
	// this means that user got msg on Workspace
	if( userConnections != NULL )
	{
		//DEBUG("Send: <%s>\n", jsonMessage );
		// register only works when there was no active desktop WS connection
		// and action is register
		if( wsMessageSent == FALSE )
		{
			DEBUG("NotificationRegister: message was not sent via WS\n");
			//switch( notification_type )
			//{
			//	case MN_force_all_devices:
				for( int i = 0; i < MAX_CONNECTIONS_PER_USER; i++ )
				{
					if( userConnections->umac_Connection[i] != NULL )
					{
						DEBUG("[MobileAppNotifyUserRegister] Send message to mobile device , conptr %p, umaID: %lu\n", userConnections->umac_Connection[i], userConnections->umac_Connection[i]->mac_UserMobileAppID );
						
						NotificationSent *lns = NotificationSentNew();
						lns->ns_NotificationID = notif->n_ID;
						lns->ns_UserMobileAppID = (FULONG)userConnections->umac_Connection[i]->mac_UserMobileAppID;
						lns->ns_RequestID = (FULONG)userConnections->umac_Connection[i];
						lns->ns_Target = MOBILE_APP_TYPE_ANDROID;
						lns->ns_Status = NOTIFICATION_SENT_STATUS_REGISTERED;
						
						NotificationManagerAddNotificationSentDB( sb->sl_NotificationManager, lns );
						
						int msgSendLength;
						
#ifdef WEBSOCKET_SEND_QUEUE
						if( notif->n_Extra )
						{ //TK-1039
							msgSendLength = snprintf( jsonMessage, reqLengith, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"%s\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Extra, notif->n_Application, lns->ns_ID, notif->n_ID );
						}
						else
						{
							msgSendLength = snprintf( jsonMessage, reqLengith, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Application, lns->ns_ID, notif->n_ID );
						}

						if( userConnections->umac_Connection[i] != NULL )
						{
							if( userConnections->umac_Connection[i]->mac_WebsocketPtr != NULL )
							{
								if( bsMobileReceivedMessage->bs_Size == 0 )
								{
									char number[ 128 ];
									int numsize = snprintf( number, sizeof(number), "%lu", userConnections->umac_Connection[i]->mac_UserMobileAppID );
									BufStringAddSize( bsMobileReceivedMessage, number, numsize );
								}
								else
								{
									char number[ 128 ];
									int numsize = snprintf( number, sizeof(number), ",%lu", userConnections->umac_Connection[i]->mac_UserMobileAppID );
									BufStringAddSize( bsMobileReceivedMessage, number, numsize );
								}
							}
							
							UserSession *us = userConnections->umac_Connection[i]->mac_UserSession;
							if( us != NULL )
							{
								Log( FLOG_INFO, "Send notification through Mobile App: Android: '%s' len %d UMAID: %lu deviceID: %s\n", jsonMessage, msgSendLength, userConnections->umac_Connection[i]->mac_UserMobileAppID, us->us_DeviceIdentity );
							}
							else
							{
								Log( FLOG_INFO, "Send notification through Mobile App No Session: Android: '%s' len %d UMAID: %lu\n", jsonMessage, msgSendLength, userConnections->umac_Connection[i]->mac_UserMobileAppID );
							}
							WriteMessageMA( userConnections->umac_Connection[i], (unsigned char*)jsonMessage, msgSendLength );
						}
#else
						if( notif->n_Extra )
						{ //TK-1039
							snprintf( jsonMessage + LWS_PRE, reqLengith-LWS_PRE, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"%s\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Extra, notif->n_Application, lns->ns_ID, notif->n_ID );
						}
						else
						{
							snprintf( jsonMessage + LWS_PRE, reqLengith-LWS_PRE, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Application, lns->ns_ID, notif->n_ID );
						}
						lws_write(userConnections->umac_Connection[i]->mac_WebsocketPtr,(unsigned char*)jsonMessage+LWS_PRE,jsonMessageLength,LWS_WRITE_TEXT);
#endif
						// adding ID's of connections which already got message
						
						
						//NotificationSentDelete( lns );
						// add NotificationSent to Notification
						lns->node.mln_Succ = (MinNode *)notif->n_NotificationsSent;
						notif->n_NotificationsSent = lns;
					}
				}
		}
		// wsMessageSent == FALSE
		else
		{
			DEBUG("[MobileAppNotifyUserRegister] Message was already sent to WS\n");
		}
		
		if( FRIEND_MUTEX_LOCK( &globalSessionRemovalMutex ) == 0 )
		{
			userConnections->umac_InUse=0;
			FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
		}
	}
	else
	{
		DEBUG("[MobileAppNotifyUserRegister] User <%s> does not have any app WS connections\n", username );
	}
	
	// select all connections without usermobileappid and store message for them in database
	
	if( wsMessageSent == FALSE )
	{
		UserMobileApp *root = MobleManagerGetMobileAppByUserPlatformAndNotInDBm( sb->sl_MobileManager, userID , MOBILE_APP_TYPE_ANDROID, USER_MOBILE_APP_STATUS_APPROVED, bsMobileReceivedMessage->bs_Buffer );
		while( root != NULL )
		{
			UserMobileApp *toDelete = root;
			root = (UserMobileApp *)root->node.mln_Succ;
			
			DEBUG("Notification for MobileDevice: %lu will be sent later\n", toDelete->uma_ID );
		
			NotificationSent *lns = NotificationSentNew();
			lns->ns_NotificationID = notif->n_ID;
			lns->ns_UserMobileAppID = (FULONG)toDelete->uma_ID;
			lns->ns_RequestID = (FULONG)toDelete->uma_ID;
			lns->ns_Target = MOBILE_APP_TYPE_ANDROID;
			lns->ns_Status = NOTIFICATION_SENT_STATUS_REGISTERED;
			NotificationManagerAddNotificationSentDB( sb->sl_NotificationManager, lns );
			
			Log( FLOG_INFO, "Notification will be send to android device, umaid: %lu\n", lns->ns_UserMobileAppID );
		
			UserMobileAppDelete( toDelete );
			NotificationSentDelete( lns );
		}
	}
	/*				
	// this way all of devices which were not avaiable during sending will get message
	// they will not get them in one case, when Notification attached to it will be removed
	 */
	BufStringDelete( bsMobileReceivedMessage );
	
	// message to user Android: "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\"}"
	// message from example to APNS: /client.py '{"auth":"72e3e9ff5ac019cb41aed52c795d9f4c","action":"notify","payload":"hellooooo","sound":"default","token":"1f3b66d2d16e402b5235e1f6f703b7b2a7aacc265b5af526875551475a90e3fe","badge":1,"category":"whatever"}'
	
	DEBUG("[MobileAppNotifyUserRegister] send message to other mobile apps, message was alerady sent? %d\n", wsMessageSent );
	
	char *jsonMessageIOS = NULL;
	int jsonMessageIosLength = reqLengith+512;
	if( wsMessageSent == FALSE && sb->sl_NotificationManager->nm_APNSCert != NULL )//&& sb->l_APNSConnection != NULL && sb->l_APNSConnection->wapns_Connection != NULL )
	{
		if( ( jsonMessageIOS = FMalloc( jsonMessageIosLength ) ) != NULL )
		{
			// on the end, list for the user should be taken from DB instead of going through all connections
			
			char *tokens = MobleManagerGetIOSAppTokensDBm( sb->sl_MobileManager, userID );
			if( tokens != NULL )
			{
				Log( FLOG_INFO, "Send notification through Mobile App: IOS '%s' : tokens %s\n", notif->n_Content, tokens );
				NotificationManagerNotificationSendIOS( sb->sl_NotificationManager, notif->n_Title, notif->n_Content, "default", 1, notif->n_Application, notif->n_Extra, tokens );
				FFree( tokens );
			}
			FFree( jsonMessageIOS );
		}
	}
	else
	{
		FERROR("Message was sent through websockets or there is no valid Apple APNS certyficate!\n");
	}
	FFree( jsonMessage );
	
	// message was not sent via Websockets, there is no need to put it into queue
	if( wsMessageSent == FALSE )
	{
		NotificationDelete( notif );
		return -1;
	}

	return 0;
}


/**
 * Notify user update
 *
 * @param username pointer to string with user name
 * @param notif pointer to Notfication structure
 * @param notifSentID id of NotificationSent from information is coming
 * @param action id of action
 * @return 0 when message was send, otherwise error number
 */
int MobileAppNotifyUserUpdate( void *lsb, const char *username, Notification *notif, FULONG notifSentID, int action )
{
	if( username == NULL )
	{
		Log( FLOG_ERROR, "[MobileAppNotifyUserUpdate]: Username is NULL!\n");
		return 1;
	}
	SystemBase *sb = (SystemBase *)lsb;
	UserMobileAppConnections *userConnections = NULL;
	NotificationSent *notifSent = NULL;
	
	// get message length
	
	unsigned int reqLengith = LWS_PRE + 512;
	
	DEBUG("[MobileAppNotifyUserUpdate] start\n");
	
	if( notif != NULL )
	{
		if( notif->n_NotificationsSent == NULL )
		{
			notif->n_NotificationsSent = NotificationManagerGetNotificationsSentDB( sb->sl_NotificationManager, notif->n_ID );
		}
	}
	else	// Notification was not provided by function, must be readed from DB
	{
		notif = NotificationManagerGetTreeByNotifSentDB( sb->sl_NotificationManager, notifSentID );
		if( notif != NULL )
		{
			NotificationSent *ln = notif->n_NotificationsSent;
			while( ln != NULL )
			{
				if( notifSentID == ln->ns_ID )
				{
					notifSent = ln;
					break;
				}
				ln = (NotificationSent *)ln->node.mln_Succ;
			}
		}
	}
	
	if( notif != NULL )
	{
		if( notif->n_Channel != NULL )
		{
			reqLengith += strlen( notif->n_Channel );
		}
		
		if( notif->n_Content != NULL )
		{
			reqLengith += strlen( notif->n_Content );
		}
		
		if( notif->n_Title != NULL )
		{
			reqLengith += strlen( notif->n_Title );
		}
		
		if( notif->n_Application != NULL )
		{
			reqLengith += strlen( notif->n_Application );
		}
		
		if( notif->n_Extra != NULL )
		{
			reqLengith += strlen( notif->n_Extra );
		}
	}
	else
	{
		FERROR("\n\n\n\nCannot find notification!\n");
		return 1;
	}
	
	// allocate memory for message
	
	char *jsonMessage = FMalloc( reqLengith );
	
	BufString *bsMobileReceivedMessage = BufStringNew();
	
	DEBUG("Buffer generated\n");
	//
	// go through all mobile devices
	//
	
	if( FRIEND_MUTEX_LOCK( &globalSessionRemovalMutex ) == 0 )
	{
		DEBUG("Hashmap get 1: %s\n", username );
		
		userConnections = CGetDataFromList( (char *)username );

		FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
	}
	DEBUG("got userConnections: %p\n", userConnections );

	// if message was already sent
	// this means that user got msg on Workspace
	if( userConnections != NULL )
	{
		DEBUG("compare actions %d = %d\n", action, NOTIFY_ACTION_READ );
		if( action == NOTIFY_ACTION_READ )
		{
			
			/*
			switch( notif->n_NotificationType )
			{
				case MN_force_all_devices:
				for( int i = 0; i < MAX_CONNECTIONS_PER_USER; i++ )
				{
					// connection which was sending timeout 
					if( user_connections->connection[i] )
					{
						NotificationSent *lnf = notif->n_NotificationsSent;
						
						while( lnf != NULL )
						{
							if( lnf->ns_RequestID == (FULONG)user_connections->connection[i]->mac_UserMobileAppID )
							{
#ifdef WEBSOCKET_SEND_QUEUE
								unsigned int jsonMessageLength = snprintf( jsonMessage, reqLengith, "{\"t\":\"notify\",\"channel\":\"%s\",\"application\":\"%s\",\"action\":\"remove\",\"id\":%lu}", notif->n_Channel, notif->n_Application, lnf->ns_ID );
						
								WriteMessageMA( user_connections->connection[i], (unsigned char*)jsonMessage, jsonMessageLength );
#else
								unsigned int jsonMessageLength = LWS_PRE + snprintf( jsonMessage + LWS_PRE, reqLengith-LWS_PRE, "{\"t\":\"notify\",\"channel\":\"%s\",\"application\":\"%s\",\"action\":\"remove\",\"id\":%lu}", notif->n_Channel, notif->n_Application, lnf->ns_ID );
								lws_write(user_connections->connection[i]->mac_WebsocketPtr,(unsigned char*)jsonMessage+LWS_PRE,jsonMessageLength,LWS_WRITE_TEXT);
#endif
								break;
							}
							lnf = (NotificationSent *) lnf->node.mln_Succ;
						}
					}
				}
				break;

				case MN_all_devices:
				for( int i = 0; i < MAX_CONNECTIONS_PER_USER; i++ )
				{
					if( user_connections->connection[i] && user_connections->connection[i]->mac_AppStatus != MOBILE_APP_STATUS_RESUMED )
					{
						NotificationSent *lnf = notif->n_NotificationsSent;
						
						while( lnf != NULL )
						{
							if( lnf->ns_RequestID == (FULONG)user_connections->connection[i]->mac_UserMobileAppID )
							{
#ifdef WEBSOCKET_SEND_QUEUE
								unsigned int jsonMessageLength = snprintf( jsonMessage, reqLengith, "{\"t\":\"notify\",\"channel\":\"%s\",\"application\":\"%s\",\"action\":\"remove\",\"id\":%lu}", notif->n_Channel, notif->n_Application, lnf->ns_ID );
						
								WriteMessageMA( user_connections->connection[i], (unsigned char*)jsonMessage, jsonMessageLength );
#else
								unsigned int jsonMessageLength = snprintf( jsonMessage + LWS_PRE, reqLengith-LWS_PRE, "{\"t\":\"notify\",\"channel\":\"%s\",\"application\":\"%s\",\"action\":\"remove\",\"id\":%lu}", notif->n_Channel, notif->n_Application, lnf->ns_ID );
								lws_write(user_connections->connection[i]->mac_WebsocketPtr,(unsigned char*)jsonMessage+LWS_PRE,jsonMessageLength,LWS_WRITE_TEXT);
#endif
								break;
							}
							lnf = (NotificationSent *) lnf->node.mln_Succ;
						}
					}
				}
				break;
				default: FERROR("**************** UNIMPLEMENTED %d\n", notif->n_NotificationType);
			}
			*/
		}
		//
		// seems noone readed message on desktop, we must inform all user channels that we have package for him
		//
		else if( action == NOTIFY_ACTION_TIMEOUT )
		{
			DEBUG("modify\n");
			for( int i = 0; i < MAX_CONNECTIONS_PER_USER; i++ )
			{
				DEBUG("[MobileAppNotifyUserUpdate]: Send to %d\n", i );
				
				// connection which was sending timeout 
				if( userConnections->umac_Connection[i] )
				{
					NotificationSent *lns = NotificationSentNew();
					lns->ns_NotificationID = notif->n_ID;
					lns->ns_UserMobileAppID = (FULONG)userConnections->umac_Connection[i]->mac_UserMobileAppID;
					lns->ns_RequestID = (FULONG)userConnections->umac_Connection[i];
					lns->ns_Target = MOBILE_APP_TYPE_ANDROID;
					lns->ns_Status = NOTIFICATION_SENT_STATUS_REGISTERED;
					NotificationManagerAddNotificationSentDB( sb->sl_NotificationManager, lns );
					
					if( bsMobileReceivedMessage->bs_Size == 0 )
					{
						char number[ 128 ];
						int numsize = snprintf( number, sizeof(number), "%lu", userConnections->umac_Connection[i]->mac_UserMobileAppID );
						BufStringAddSize( bsMobileReceivedMessage, number, numsize );
					}
					else
					{
						char number[ 128 ];
						int numsize = snprintf( number, sizeof(number), ",%lu", userConnections->umac_Connection[i]->mac_UserMobileAppID );
						BufStringAddSize( bsMobileReceivedMessage, number, numsize );
					}
						
					unsigned int jsonMessageLength = 0;
#ifdef WEBSOCKET_SEND_QUEUE
					if( notif->n_Extra )
					{ //TK-1039
						jsonMessageLength = snprintf( jsonMessage, reqLengith, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"%s\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Extra, notif->n_Application, lns->ns_ID, notif->n_ID );
					}
					else
					{
						jsonMessageLength = snprintf( jsonMessage, reqLengith, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Application, lns->ns_ID, notif->n_ID );
					}
					
					if( userConnections->umac_Connection[i] != NULL )
					{
						DEBUG("[MobileAppNotifyUserUpdate]: Connection pointer: %p\n", userConnections->umac_Connection[i] );
						//Log( FLOG_INFO, "Send notification (update) through Mobile App: Android '%s' len %d \n", jsonMessage, jsonMessageLength );
						
						UserSession *us = userConnections->umac_Connection[i]->mac_UserSession;
						if( us != NULL )
						{
							Log( FLOG_INFO, "Send notification (update) through Mobile App: Android: '%s' len %d UMAID: %lu deviceID: %s\n", jsonMessage, jsonMessageLength, userConnections->umac_Connection[i]->mac_UserMobileAppID, us->us_DeviceIdentity );
						}
						else
						{
							Log( FLOG_INFO, "Send notification (update) through Mobile App No Session: Android: '%s' len %d UMAID: %lu\n", jsonMessage, jsonMessageLength, userConnections->umac_Connection[i]->mac_UserMobileAppID );
						}
						
						WriteMessageMA( userConnections->umac_Connection[i], (unsigned char*)jsonMessage, jsonMessageLength );
#else
						if( notif->n_Extra )
						{ //TK-1039
							jsonMessageLength = snprintf( jsonMessage + LWS_PRE, reqLengith-LWS_PRE, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"%s\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Extra, notif->n_Application, lns->ns_ID, notif->n_ID );
						}
						else
						{
							jsonMessageLength = snprintf( jsonMessage + LWS_PRE, reqLengith-LWS_PRE, "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu,\"notifid\":%lu,\"source\":\"notification\"}", notif->n_Channel, notif->n_Content, notif->n_Title, notif->n_Application, lns->ns_ID, notif->n_ID );
						}
					
						if( userConnections->umac_Connection[i] != NULL && userConnections->umac_Connection[i]->mac_WebsocketPtr != NULL )
						{
							lws_write( userConnections->umac_Connection[i]->mac_WebsocketPtr,(unsigned char*)jsonMessage+LWS_PRE,jsonMessageLength,LWS_WRITE_TEXT);
						}
#endif
					} // if( userConnections->umac_Connection[i] )
					
					NotificationSentDelete( lns );
				}
				
			}
			
		}
		
		if( FRIEND_MUTEX_LOCK( &globalSessionRemovalMutex ) == 0 )
		{
			userConnections->umac_InUse=0;
			FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
		}
	}
	else
	{
		FERROR("User <%s> does not have any app WS connections\n", username);
	}
	
	// select all connections without usermobileappid and store message for them in database
	// new messages must be created only when timeout appear
	if( action == NOTIFY_ACTION_TIMEOUT )
	{
		FULONG userID = UMGetUserIDByName( sb->sl_UM, username );
		UserMobileApp *root = MobleManagerGetMobileAppByUserPlatformAndNotInDBm( sb->sl_MobileManager, userID , MOBILE_APP_TYPE_ANDROID, USER_MOBILE_APP_STATUS_APPROVED, bsMobileReceivedMessage->bs_Buffer );
	
		Log( FLOG_INFO, "Send notification (update) through Mobile App: Android, notifications will not be registered for ID: %s\n", bsMobileReceivedMessage->bs_Buffer );
	
		while( root != NULL )
		{
			UserMobileApp *toDelete = root;
			root = (UserMobileApp *)root->node.mln_Succ;
		
			DEBUG("Notification for MobileDevice: %lu will be sent later\n", toDelete->uma_ID );
	
			NotificationSent *lns = NotificationSentNew();
			lns->ns_NotificationID = notif->n_ID;
			lns->ns_UserMobileAppID = (FULONG)toDelete->uma_ID;
			lns->ns_RequestID = (FULONG)toDelete->uma_ID;
			lns->ns_Target = MOBILE_APP_TYPE_ANDROID;
			lns->ns_Status = NOTIFICATION_SENT_STATUS_REGISTERED;
			NotificationManagerAddNotificationSentDB( sb->sl_NotificationManager, lns );
	
			UserMobileAppDelete( toDelete );
			NotificationSentDelete( lns );
		}
	}
	/*				
	// this way all of devices which were not avaiable during sending will get message
	// they will not get them in one case, when Notification attached to it will be removed
	 */
	BufStringDelete( bsMobileReceivedMessage );
	
	//FRIEND_MUTEX_UNLOCK( &globalSessionRemovalMutex );
	
	// message to user Android: "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\"}"
	// message from example to APNS: /client.py '{"auth":"72e3e9ff5ac019cb41aed52c795d9f4c","action":"notify","payload":"hellooooo","sound":"default","token":"1f3b66d2d16e402b5235e1f6f703b7b2a7aacc265b5af526875551475a90e3fe","badge":1,"category":"whatever"}'
	
	DEBUG("[MobileAppNotifyUserUpdate]: send message to other mobile apps\n");
	
	char *jsonMessageIOS;
	int jsonMessageIosLength = reqLengith+512;
	//if( sb->l_APNSConnection != NULL )&& sb->l_APNSConnection->wapns_Connection != NULL )
	if( sb->sl_NotificationManager->nm_APNSCert != NULL )
	{
		FULONG userID = 0;
		User *usr = UMGetUserByName( sb->sl_UM, username );
		if( usr != NULL )
		{
			userID = usr->u_ID;
			//UserDelete( usr );	// user cannot be deleted from list!
		}
		
		if( ( jsonMessageIOS = FMalloc( jsonMessageIosLength ) ) != NULL )
		{
			UserMobileApp *lmaroot = MobleManagerGetMobileAppByUserPlatformDBm( sb->sl_MobileManager, userID, MOBILE_APP_TYPE_IOS, USER_MOBILE_APP_STATUS_APPROVED, FALSE );
			UserMobileApp *lma = lmaroot;
			
			
			if( action == NOTIFY_ACTION_READ )
			{
				// we should send message to all other devices that message was read
				/*
				while( lma != NULL )
				{
					NotificationSent *lns = NotificationSentNew();
					lns->ns_NotificationID = notif->n_ID;
					lns->ns_UserMobileAppID = lma->uma_ID;
					lns->ns_RequestID = (FULONG)lma;
					lns->ns_Target = MOBILE_APP_TYPE_IOS;
					lns->ns_Status = NOTIFICATION_SENT_STATUS_READ;
					NotificationManagerAddNotificationSentDB( sb->sl_NotificationManager, lns );
					
					Log( FLOG_INFO, "Send notification (update) through Mobile App: IOS '%s' iostoken: %s\n", notif->n_Content, lma->uma_AppToken );
					
					NotificationManagerNotificationSendIOS( sb->sl_NotificationManager, notif->n_Content, "default", 1, notif->n_Application, lma->uma_AppToken );

					NotificationSentDelete( lns );

					lma = (UserMobileApp *)lma->node.mln_Succ;
				}*/
			}
			//
			// seems noone readed message on desktop, we must inform all user channels that we have package for him
			//
			else if( action == NOTIFY_ACTION_TIMEOUT )
			{
				while( lma != NULL )
				{
					NotificationSent *lns = NotificationSentNew();
					lns->ns_NotificationID = notif->n_ID;
					lns->ns_UserMobileAppID = lma->uma_ID;
					lns->ns_RequestID = (FULONG)lma;
					lns->ns_Target = MOBILE_APP_TYPE_IOS;
					lns->ns_Status = NOTIFICATION_SENT_STATUS_REGISTERED;
					NotificationManagerAddNotificationSentDB( sb->sl_NotificationManager, lns );
					
					Log( FLOG_INFO, "Send notification (update) through Mobile App: IOS '%s' iostoken: %s\n", notif->n_Content, lma->uma_AppToken );
					
					NotificationManagerNotificationSendIOS( sb->sl_NotificationManager, notif->n_Title, notif->n_Content, "default", 1, notif->n_Application, notif->n_Extra, lma->uma_AppToken );
					/*
					int msgsize = snprintf( jsonMessageIOS, jsonMessageIosLength, "{\"auth\":\"%s\",\"action\":\"notify\",\"payload\":\"%s\",\"sound\":\"default\",\"token\":\"%s\",\"badge\":1,\"category\":\"whatever\",\"application\":\"%s\",\"action\":\"register\",\"id\":%lu}", sb->l_AppleKeyAPI, notif->n_Content, lma->uma_AppToken, notif->n_Application, lns->ns_ID );
			
					WebsocketClientSendMessage( sb->l_APNSConnection->wapns_Connection, jsonMessageIOS, msgsize );
					*/
					
					NotificationSentDelete( lns );

					lma = (UserMobileApp *)lma->node.mln_Succ;
				}
			} // notifSentID == 0
			
			UserMobileAppDeleteAll( lmaroot );

			FFree( jsonMessageIOS );
		}
	}
	else
	{
		INFO("[MobileAppNotifyUserUpdate]: No A!\n");
	}
	
	FFree( jsonMessage );
	
	//NotificationDelete( notif );
	
	return 0;
}
