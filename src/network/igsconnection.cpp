#include <string.h>
#include "igsconnection.h"
#include "consoledispatch.h"
#include "roomdispatch.h"
#include "boarddispatch.h"
#include "gamedialogdispatch.h"
#include "talkdispatch.h"
#include "dispatchregistries.h"
#include "gamedialogflags.h"
#include "playergamelistings.h"

IGSConnection::IGSConnection(class NetworkDispatch * _dispatch, const class ConnectionInfo & info)
{
	dispatch = _dispatch;
	if(openConnection(info))
	{
		authState = LOGIN;
		username = QString(info.user);
		password = QString(info.pass);
	}
	else
		qDebug("Can't open Connection\n");	//throw error?

	writeReady = false;
	keepAliveTimer = 0;
	textCodec = QTextCodec::codecForLocale();
	registerMsgHandlers();
}

IGSConnection::~IGSConnection()
{
	qDebug("Destroying IGS connection\n");
	/* Maybe closeConnection shouldn't be here?
	 * at any rate, if there's been an error it
	 * really shouldn't be here */
	sendDisconnect();
	closeConnection();
	delete msgHandlerRegistry;
}

void IGSConnection::sendText(QString text)
{
	// FIXME We're getting some nonsense on the end of this sometimes for
	// some reason
	//text += "\r\n";
	qDebug("sendText: %s", text.toLatin1().constData());
	QByteArray raw = textCodec->fromUnicode(text);
	bool wr;
	if(writeReady)
		wr = true;
	else
		wr = false;
	if(write(raw.data(), raw.size()) < 0)
		qWarning("*** failed sending to host: %s", raw.data());
	else
	{
		if(wr)
			writeReady = true;
		//write("\r\n", 2);
	}
}

/* This is kind of ugly, but we can't add sending return-newline to
 * superclass, which means either reallocing text here to send both
 * or this little flag trick to sneak in a second write when we
 * got the first through.  Note that the real solution would be
 * to change every sendText command in this file to have a "\r\n"
 * on it.  That is sort of the better way. FIXME 
 * Note also that network connection does readLine in writeFromBuffer
 * and also write an additional newline...*/
void IGSConnection::sendText(const char * text)
{
	if(write(text, strlen(text)) < 0)
		qWarning("*** failed sending to host: %s", text);
}

void IGSConnection::sendDisconnect(void)
{
	sendText("exit\r\n");
}

/* What about a room_id?? */
void IGSConnection::sendMsg(unsigned int game_id, QString text)
{
	GameData * g = getGameData(game_id);
	switch(g->gameType)
	{
		case modeReview:
		case modeMatch:
			sendText("say " + text + "\r\n");
			break;
		case modeObserve:
			sendText("kibitz " + QString::number(game_id) + " " + text + "\r\n");
			break;
		default:
			qDebug("no game type");
			break;
	}
}

void IGSConnection::sendMsg(const PlayerListing & player, QString text)
{
	/* Taken from MainWindow::slot_talkTo */
	sendText("tell " + player.name + " " + text + "\r\n");
}

void IGSConnection::sendToggle(const QString & param, bool val)
{
	QString value;
	if (val)
		value = " true";
	else
		value = " false";
	sendText("toggle " + param + " " + value + "\r\n");
}

/* If we can observe something besides games,
 * like rooms or people, then this name is a bit
 * iffy too... */
void IGSConnection::sendObserve(const GameListing & game)
{
	unsigned int game_id = game.number;
	if(getIfBoardDispatch(game_id))		//don't observe twice, it unobserves
		return;
	protocol_save_int = game_id;	//since this isn't always reported back
	sendText("observe " + QString::number(game_id) + "\r\n");
}

void IGSConnection::stopObserving(const GameListing & game)
{
	sendText("unobserve " + QString::number(game.number) + "\r\n");
}

void IGSConnection::stopReviewing(const GameListing & game)
{
	sendText("review quit " + QString::number(game.number) + "\r\n");
}

void IGSConnection::sendStatsRequest(const PlayerListing & opponent)
{
	sendText("stats " + opponent.name + "\r\n");
}

void IGSConnection::sendPlayersRequest(void)
{
	sendText("userlist\r\n");
}

void IGSConnection::sendGamesRequest(void)
{
	sendText("games\r\n");
}

void IGSConnection::sendRoomListRequest(void)
{
	sendText("room\r\n");
}

void IGSConnection::sendJoinRoom(const RoomListing & room, const char * /*password*/)
{
	sendText("join " + QString::number(room.number) + "\r\n");
	qDebug("Joining %d", room.number);
	
	RoomDispatch * roomdispatch = getDefaultRoomDispatch();
	roomdispatch->clearPlayerList();
	roomdispatch->clearGamesList();
	sendPlayersRequest();
	sendGamesRequest();
}

void IGSConnection::sendMatchInvite(const PlayerListing & player)
{
	MatchRequest * m = 0;
	/* No match Invites, just popup the dialog */
	GameDialogDispatch * gd = getGameDialogDispatch(player);
	if(player.nmatch)
	{
		const PlayerListing us = getOurListing();
		m = new MatchRequest();
		m->opponent = player.name;
		m->opponent_id = player.id;
		m->their_rank = player.rank;
		m->our_name = us.name;
		m->our_rank = us.rank;
		
		m->timeSystem = player.nmatch_timeSystem;
		m->maintime = player.nmatch_timeMin;
		m->periodtime = player.nmatch_BYMin;
		m->stones_periods = 25;
		m->nmatch_timeMax = player.nmatch_timeMax;
		m->nmatch_BYMax = player.nmatch_BYMax;
		if(!player.nmatch_handicapMin)
			m->handicap = 1;
		else
			m->handicap = player.nmatch_handicapMin;
		m->nmatch_handicapMax = player.nmatch_handicapMax;
		m->opponent_is_challenger = false;
		//mr->first_offer = false;	//because we don't want to toggle to match from nmatch
		//mr->rated = false;
	}
	gd->recvRequest(m, getGameDialogFlags());
}

/* I'm thinking you can only play one game at a time on IGS,
 * but the game_ids are there in case it changes its mind */
void IGSConnection::adjournGame(const GameListing &/*game_id*/)
{
	/* double check this one */
	sendText("adjourn\r\n");
}

void IGSConnection::sendMove(unsigned int game_id, MoveRecord * move)
{
	switch(move->flags)
	{
		case MoveRecord::PASS:
			sendText("pass\r\n");
			break;
		case MoveRecord::REQUESTUNDO:
			sendText("undoplease\r\n");	//so polite
			break;
		case MoveRecord::UNDO:
			sendText("undo\r\n");
			break;
		case MoveRecord::REFUSEUNDO:
			sendText("noundo\r\n");
			break;
		case MoveRecord::RESIGN:
			sendText("resign\r\n");
			break;
		case MoveRecord::DONE_SCORING:
			sendText("done\r\n");
			break;
		case MoveRecord::NONE:
		case MoveRecord::REMOVE:	//handled simply
		case MoveRecord::UNREMOVE:
			{
			char c1 = move->x - 1 + 'A';
			if(move->x > 8)		// no I in IGS
				c1++;
			GameData * g = getGameData(game_id);
			int c2 = g->board_size + 1 - move->y; 
			/* Why do we send the id here but
			 * not with the others?  Can we play
			 * multiple games?? */
			sendText(QString(c1) + QString::number(c2) + " " + QString::number(game_id) + "\r\n");	
			}
			break;
		default:
			qDebug("IGSConnection: unhandled  type\n");
			break;
	}
}

void IGSConnection::sendMatchRequest(MatchRequest * mr)
{
	QString color;
	switch(mr->color_request)
	{
		case MatchRequest::BLACK:
			color = " B ";
			break;
		case MatchRequest::WHITE:
			color = " W ";
			break;
		case MatchRequest::NIGIRI:
			color = " N ";
			break;		
	}
	/* FIXME nmatch almost shouldn't be a setting on the match
	 * request.  We should check the settings here and if they're
	 * fairly simple, change it to match in order to be compatible
	 * with more other clients... we need to check if this is
	 * our first offer as well we don't want to change to match
	 * mid negotiation.
	 * nmatch does have to be a setting though so we can't change
	 * it through negotiation on game dialog*/
	if(mr->first_offer)
	{
		mr->first_offer = false;
		if(mr->timeSystem == canadian && color != " N ")
			mr->nmatch = false;
	}
	if(mr->nmatch)
	{
		if(mr->timeSystem == byoyomi)
			sendText("nmatch " + mr->opponent +
				color +
				QString::number(mr->handicap) + " " +
				QString::number(mr->board_size) + " " +
				QString::number(mr->maintime) + " " +
				QString::number(mr->periodtime) + " " +
				QString::number(mr->stones_periods) + " " +
				"0 0 0\r\n");
		else if(mr->timeSystem == canadian)
			sendText("nmatch " + mr->opponent +
					color +
					QString::number(mr->handicap) + " " +
					QString::number(mr->board_size) + " " +
					QString::number(mr->maintime) + " " +
					QString::number(mr->periodtime) + " " +
					QString::number(mr->stones_periods) + " " +
					"0 0 0\r\n");
		
	}
	else
	{
		sendText("match " + mr->opponent +
				color +
				QString::number(mr->board_size) + " " +
				QString::number(mr->maintime / 60) + " " +
				QString::number(mr->periodtime) + "\r\n");
	}
	match_playerName = mr->opponent;
}

unsigned long IGSConnection::getGameDialogFlags(void)
{
	//GDF_STONES25_FIXED is only set in "match" not "nmatch"
	//there's also pmatch and tdmatch now I think
	return (GDF_CANADIAN | GDF_BYOYOMI | GDF_CANADIAN300 | GDF_BY_CAN_MAIN_MIN |
		 GDF_NIGIRI_EVEN | GDF_KOMI_FIXED6
			/*| GDF_STONES25_FIXED*/);
}

void IGSConnection::declineMatchOffer(const PlayerListing & opponent)
{
	// also possibly a "withdraw" message before opponent has responded FIXME
	sendText("decline " + opponent.name + "\r\n");
}

void IGSConnection::acceptMatchOffer(const PlayerListing & /*opponent*/, MatchRequest * mr)
{
	/* For IGS, we just send the same match request, assuming it hasn't changed.
	* It would be bad to have the connection code hold the match request, so
	* we'll have the gamedialogdispatch pass it along and we will trust
	* that its the same.*/
	sendMatchRequest(mr);
}

/* Perhaps IGS does not allow multiple games, but this here will be trickier
 * possibly, with other services that might since there's no game id here */
void IGSConnection::sendAdjournRequest(void)
{
	sendText("adjourn\r\n");
}

void IGSConnection::sendAdjourn(void)
{
	sendText("adjourn\r\n");
}

void IGSConnection::sendRefuseAdjourn(void)
{
	sendText("decline adjourn\r\n");
}

/* There's honestly not much of a reason why we moved this out
 * of the mainwindow_server except that it doesn't really belong
 * there and we don't want the mainwindow ui code to be coupled
 * to the IGS code.  That said, since we've yet to add
 * another seek facility to a different protocol, its completely
 * tailored to IGS, but I guess its a start */
void IGSConnection::sendSeek(SeekCondition * s)
{
	//seek entry 1 19 5 3 0
	QString send_seek = "seek entry " + 
			QString::number(s->number) + 
			" 19 " + s->strength_wished + "\r\n";
	sendText(send_seek);
}

void IGSConnection::sendSeekCancel(void)
{
	sendText("seek entry_cancel\r\n");
}

/*void IGSConnection::setAccountAttrib(AccountAttib * aa)
{
		
}*/

void IGSConnection::handlePendingData(newline_pipe <unsigned char> * p)
{
	char * c;
	int bytes;

	switch(authState)
	{
		case LOGIN:
			bytes = p->canRead();
			if(bytes)
			{
				c = new char[bytes];
				p->read((unsigned char *)c, bytes);
				handleLogin(QString(c));
				delete[] c;
			}
			break;
		case PASSWORD:
			bytes = p->canRead();
			if(bytes)
			{
				c = new char[bytes];
				p->read((unsigned char *)c, bytes);
				handlePassword(QString(c));
				delete[] c;
			}
			break;
		case SESSION:
			while((bytes = p->canReadLine()))
			{
				c = new char[bytes + 1];
				bytes = p->readLine((unsigned char *)c, bytes);
				QString unicodeLine = textCodec->toUnicode(c);
				//unicodeLine.truncate(unicodeLine.length() - 1);
				handleMessage(unicodeLine);
				delete[] c;
			}
			break;
		case AUTH_FAILED:
			qDebug("Auth failed\n");
			break;
	}
}

void IGSConnection::handleLogin(QString msg)
{
	if(msg.contains("Login:") > 0)
	{
		qDebug("Login found\n");
		writeReady = true;
		QString u = username + "\r\n";
		sendText(u.toLatin1().constData());	
		authState = PASSWORD;
	}
	else if(msg.contains("sorry") > 0)
	{
		authState = AUTH_FAILED;
		if(console_dispatch)
			console_dispatch->recvText("Sorry");
	}
	else if(console_dispatch)
		console_dispatch->recvText(msg.toLatin1().constData());
}

void IGSConnection::handlePassword(QString msg)
{
	qDebug(":%d %s\n", msg.size(), msg.toLatin1().constData());
	if(msg.contains("Password:") > 0 || msg.contains("1 1") > 0)
	{
		qDebug("Password or 1 1 found\n");
		writeReady = true;
		QString p = password + "\r\n";
		sendText(p.toLatin1().constData());	
		authState = SESSION;
	}
}

bool IGSConnection::isReady(void)
{
	if(authState == SESSION)
		return 1;
	else
		return 0;
}

void IGSConnection::onReady(void)
{
	if(firstonReadyCall)
	{
		firstonReadyCall = 0;
	/* This gets called too much, we need a better
	 * way to call it */
	/* also needs to be earlier */
	/* Below is kind of round-about.  We need the account name in order to make our
	* name in the players list blue.  We could also use something similar to 
	* affect colors of observed or played games, although I'm not sure why we'd
	* do any of this.  But it means setting the account name on the listview model
	* from the room.  The room has no other real use for the account name, so we
	* pull it from the connection just for that.  Its a lot of misdirection just
	* preserve the encapsulation but I suppose its okay for now.  We might
	* fix it along the lines of the above at some point. */
	/* Specifically, when we figure out how we're going to deal with rooms in the
	* future, then we'll find a better place to do this and set the connection
	* etc.. the idea of a "DefaultRoom" will sort of drop away. FIXME */
	/* There's a bug here where a person who's name is "" gets painted blue FIXME */
	//getDefaultRoomDispatch()->setAccountName(username);
	
	/* We can't seem to really send automatic commands.
	 * We get banned for a time and we get socket errors
	 * so we need to check for readabililty and I wonder
	 * if we need another newline pipe all around to buffer
	 * outgoing data until socket can be written to! 
	 * ... or maybe everything is fine.*/
	QString v = "id qGo2v" + QString(VERSION) + "\r\n";
	sendText(v.toLatin1().constData());
	//sendText("toggle newrating\r\n");
	
	//sendText("toggle open false\r\n");
	sendText("toggle newundo on\r\n");
	sendText("toggle client on\r\n");		//adds type codes
	sendText("toggle nmatch on\r\n");		//allows nmatch
	sendText("toggle seek on\r\n");

	sendPlayersRequest();
	sendGamesRequest();
	dispatch->recvRoomListing(new RoomListing(0, "Lobby"));
	sendRoomListRequest();
	
	sendText("seek config_list\r\n");
	sendNmatchParameters();
	qDebug("Ready!\n");
	}
	writeReady = true;
	writeFromBuffer();
}

/* In case we want to add updates every so often */
void IGSConnection::timerEvent(QTimerEvent* e)
{
	//i.e., startTimer somewhere customizable and then
	//sendPlayersRequest(), sendGamesRequest()
	if(e->timerId() == keepAliveTimer)
	{
		sendText("ayt\r\n");
		return;
	}
}

void IGSConnection::setKeepAlive(int seconds)
{
	if(keepAliveTimer)
		killTimer(keepAliveTimer);
	
	if(seconds > 0)
		keepAliveTimer = startTimer(seconds * 1000);
	else
		keepAliveTimer = 0;
}

/* Because the IGS protocol is garbage, we have to break encapsulation here
 * and in BoardDispatch */
BoardDispatch * IGSConnection::getBoardFromAttrib(QString black_player, unsigned int black_captures, float black_komi, QString white_player, unsigned int white_captures, float white_komi)
{
	BoardDispatch * board;
	std::map<unsigned int, class BoardDispatch *>::iterator i;
	std::map<unsigned int, class BoardDispatch *> * boardDispatchMap =
		boardDispatchRegistry->getRegistryStorage();
	for(i = boardDispatchMap->begin(); i != boardDispatchMap->end(); i++)
	{
		board = i->second;
		if(board->isAttribBoard(black_player, black_captures, black_komi, white_player, white_captures, white_komi))
			return board;
	}
	return NULL;
}

BoardDispatch * IGSConnection::getBoardFromOurOpponent(QString opponent)
{
	BoardDispatch * board;
	std::map<unsigned int, class BoardDispatch *>::iterator i;
	std::map<unsigned int, class BoardDispatch *> * boardDispatchMap =
		boardDispatchRegistry->getRegistryStorage();
	/* Parser may supply our name from the IGS protocol... this is ugly
	 * but I'm really just trying to reconcile what I think a real
 	 * protocol would be like with the IGS protocol */
	if(opponent == username)
		opponent = "";
	for(i = boardDispatchMap->begin(); i != boardDispatchMap->end(); i++)
	{
		board = i->second;
		if(board->isOpponentBoard(username, opponent))
			return board;
	}
	return NULL;
}

/* Kind of ugly here FIXME.
 * Really everything should be returning references
 * or everything pointers, but there's a lot of different
 * stuff out there, so we'd really need to check every instance
 * of usage of PLayerListing and figure out what's best */
const PlayerListing & IGSConnection::getOurListing(void)
{
	PlayerListing * p;
	p = getDefaultRoomDispatch()->getPlayerListing(getUsername());
	return *p;
}

void IGSConnection::requestGameInfo(unsigned int game_id)
{
	char string[20];
	snprintf(string, 20, "moves %d\r\n", game_id);
	sendText(string);
	snprintf(string, 20, "all %d\r\n", game_id);
	sendText(string);
}

void IGSConnection::requestGameStats(unsigned int game_id)
{
	qDebug("requestGameStats");
	char string[20];
	snprintf(string, 20, "game %d\r\n", game_id);
	sendText(string);
}

int IGSConnection::time_to_seconds(const QString & time)
{
	QRegExp re = QRegExp("([0-9]{1,2}):([0-9]{1,2})");
	int min, sec;
	
	if(re.indexIn(time) >= 0)
	{
		min = re.cap(1).toInt();
		sec = re.cap(2).toInt();	
	}
	else
		qDebug("Bad time string");
	
	return (60 * min) + sec;
}

/* formulas come from old MainWindow::rkToKey for sorting.
 * might be worth fixing them to match some real score
 * system*/
/* This is a kind of utility function but I'm not sure where to
 * put it yet.  FIXME
 * If I had to guess, the best place for it is probably on
 * a rank object or something used in messages... but we don't
 * want to overload those.*/
unsigned int IGSConnection::rankToScore(QString rank)
{
	if(rank == "NR")
		return 0;
	if(rank == "BC")		//according to IGS value of 23k
		return 800;	

	QString buffer = rank;
	//buffer.replace(QRegExp("[pdk+?\\*\\s]"), "");
	buffer.replace(QRegExp("[pdk+?]"), "");
	int ordinal = buffer.toInt();
	unsigned int score;

	if(rank.contains("k"))
		score = (31 - ordinal) * 100;
	else if(rank.contains("d"))
		score = 3000 + (ordinal * 100);
	else if(rank.contains("p"))
		score = 3600 + (ordinal * 100);
	else
		return 0;
	if(rank.contains("?"))
		score--;
	return score;
}

/*
* on IGS, sends the 'nmatch'time, BY, handicap ranges
* command syntax : "nmatchrange 	BWN 	0-9 19-19	 60-60 		60-3600 	25-25 		0 0 0-0"
*				(B/W/ nigiri)	Hcp Sz	   Main time (secs)	BY time (secs)	BY stones	Koryo time
*/
void IGSConnection::sendNmatchParameters(void)
{
	QString c = "nmatchrange ";
	QSettings settings;

	c.append(preferences.nmatch_black ? "B" : "");
	c.append(preferences.nmatch_white ? "W" : "");
	c.append(preferences.nmatch_nigiri ? "N" : "");
	c.append(" 0-");
	c.append(preferences.nmatch_handicap);
	c.append(" ");
	c.append(QString::number(preferences.default_size));
	c.append("-19 ");
	
	c.append(QString::number(settings.value("DEFAULT_TIME").toInt()*60));
	c.append("-");
	c.append(QString::number(settings.value("NMATCH_MAIN_TIME").toInt()*60));
	c.append(" ");
	c.append(QString::number(settings.value("DEFAULT_BY").toInt()*60));
	c.append("-");
	c.append(QString::number(settings.value("NMATCH_BYO_TIME").toInt()*60));
	c.append(" 25-25 0 0 0-0\r\n");
	qDebug("nmatch string %s: ", c.toLatin1().constData());
	sendText(c);
}

bool IGSConnection::readyToWrite(void)
{
	if(writeReady)
	{
		writeReady = false;
		return true;
	}
	return false;
}

void IGSConnection::handleMessage(QString msg)
{
	MsgHandler * mh;
	unsigned int type = 0;
	//qDebug(msg.toLatin1().constData());	
	/*if(sscanf(msg.toLatin1().constData(), "%d", &type) != 1)
	{
		  qDebug("No number");
		  return;
	}*/
	if(msg[0].toLatin1() == '\n')
		return;
	if(msg[0].toLatin1() >= '0' && msg[0].toLatin1() <= '9')
	{
		type = (int)msg[0].toLatin1() - '0';
	}
	if(msg[1].toLatin1() >= '0' && msg[1].toLatin1() <= '9')
	{
		type *= 10;
		type += (int)msg[1].toLatin1() - '0';
	}
	if(!type)
	{
		//line = line.remove(0, 2).trimmed();
		//if(msg.size() > 4)
		//	qDebug("***%02x %02x %02x %02x", msg[msg.size() - 1].toLatin1(), msg[msg.size() - 2].toLatin1(), msg[msg.size() -3].toLatin1(), msg[msg.size() -4].toLatin1());
		
		if(msg[3].toLatin1() == '1')
		{
			msg = msg.remove(0,2).trimmed();
			mh = getMsgHandler(1);
			if(mh)
				mh->handleMsg(msg);
			return;
		}
		if(msg.size() > 1)
		{
			//additional newline unnecessary  //0a0d
			if(msg[msg.size() - 1].toLatin1() == 0x0a)
				msg.remove(msg.size() - 2, msg.size()).trimmed();
		}
		if(console_dispatch)
			console_dispatch->recvText(msg.toLatin1().constData());
		return;
	}
	//qDebug("***Type %d %c %c",type, msg[0], msg[1]);
	mh = getMsgHandler(type);
	if(mh)
		mh->handleMsg(msg);
}

#define IGS_LOGINMSG		0
#define IGS_PROMPT		1
#define IGS_BEEP		2
#define IGS_DOWN		4
#define IGS_ERROR		5
#define IGS_GAMES		7
#define IGS_FILE		8
#define IGS_INFO		9
#define IGS_KIBITZ		11
#define IGS_MESSAGES		14
#define IGS_MOVE		15
#define IGS_SAY			19
#define IGS_SCORE_M		20
#define IGS_SHOUT		21
#define IGS_STATUS		22
#define IGS_STORED		23
#define IGS_TELL		24
#define IGS_THIST		25
#define IGS_WHO			27
#define IGS_UNDO		28
#define IGS_YELL		32
#define IGS_AUTOMATCH		36
#define IGS_SERVERINFO		39
#define IGS_DOT			40
#define IGS_USERLIST		42
#define IGS_REMOVED		49
#define IGS_INGAMESAY		51
#define IGS_ADJOURNDECLINED	53
#define IGS_REVIEW		56
#define IGS_SEEK		63
void IGSConnection::registerMsgHandlers(void)
{
	qDebug("IGS registering msghandlers");
	msgHandlerRegistry = new MsgHandlerRegistry();
	msgHandlerRegistry->setEntry(IGS_LOGINMSG, new IGS_loginmsg(this));
	msgHandlerRegistry->setEntry(IGS_PROMPT, new IGS_prompt(this));
	msgHandlerRegistry->setEntry(IGS_BEEP, new IGS_beep(this));
	msgHandlerRegistry->setEntry(IGS_DOWN, new IGS_down(this));
	msgHandlerRegistry->setEntry(IGS_ERROR, new IGS_error(this));
	msgHandlerRegistry->setEntry(IGS_GAMES, new IGS_games(this));
	msgHandlerRegistry->setEntry(IGS_FILE, new IGS_file(this));
	msgHandlerRegistry->setEntry(IGS_INFO, new IGS_info(this));
	msgHandlerRegistry->setEntry(IGS_KIBITZ, new IGS_kibitz(this));
	msgHandlerRegistry->setEntry(IGS_MESSAGES, new IGS_messages(this));
	msgHandlerRegistry->setEntry(IGS_MOVE, new IGS_move(this));
	msgHandlerRegistry->setEntry(IGS_SAY, new IGS_say(this));
	msgHandlerRegistry->setEntry(IGS_INGAMESAY, new IGS_say(this));
	msgHandlerRegistry->setEntry(IGS_SCORE_M, new IGS_score_m(this));
	msgHandlerRegistry->setEntry(IGS_SHOUT, new IGS_shout(this));
	msgHandlerRegistry->setEntry(IGS_STATUS, new IGS_status(this));
	msgHandlerRegistry->setEntry(IGS_STORED, new IGS_stored(this));
	msgHandlerRegistry->setEntry(IGS_TELL, new IGS_tell(this));
	msgHandlerRegistry->setEntry(IGS_THIST, new IGS_thist(this));
	msgHandlerRegistry->setEntry(IGS_WHO, new IGS_who(this));
	msgHandlerRegistry->setEntry(IGS_UNDO, new IGS_undo(this));
	msgHandlerRegistry->setEntry(IGS_YELL, new IGS_yell(this));
	msgHandlerRegistry->setEntry(IGS_AUTOMATCH, new IGS_automatch(this));
	msgHandlerRegistry->setEntry(IGS_SERVERINFO, new IGS_serverinfo(this));
	msgHandlerRegistry->setEntry(IGS_DOT, new IGS_dot(this));
	msgHandlerRegistry->setEntry(IGS_USERLIST, new IGS_userlist(this));
	msgHandlerRegistry->setEntry(IGS_REMOVED, new IGS_removed(this));
	msgHandlerRegistry->setEntry(IGS_ADJOURNDECLINED, new IGS_adjourndeclined(this));
	msgHandlerRegistry->setEntry(IGS_REVIEW, new IGS_review(this));
	msgHandlerRegistry->setEntry(IGS_SEEK, new IGS_seek(this));
}

MsgHandler * IGSConnection::getMsgHandler(unsigned int type)
{
	MsgHandler * mh = msgHandlerRegistry->getEntry(type);
	if(!mh)
		qDebug("Strange type: %d", type);
	return mh;
}

/* IGS Protocol messages */
/* I know WING has these 0 messages... move to WING if specific FIXME*/
void IGS_loginmsg::handleMsg(QString line)
{
	/* These seem to have extra newlines on them */
	line.remove(line.length() - 1, line.length()).trimmed();
	if(line.length() > 1)
	{
		ConsoleDispatch * console = connection->getConsoleDispatch();
		if(console)
			console->recvText(line.toLatin1().constData());
	}
}


void IGS_prompt::handleMsg(QString line)
{
	line = line.remove(0, 2).trimmed();
	/* There's some other ones here, that might be an issue, besides 5 and 8,
	 * we should consider just calling connection->onReady() here */
	if(line.contains("5") || line.contains("6") || line.contains("8"))
	{
	}
		connection->onReady();
/* From NNGS source:
#define STAT_LOGON	0	// Unused
#define STAT_PASSWORD	1	// Unused
#define STAT_PASSWORD_NEW	2	// Unused
#define STAT_PASSWORD_CONFIRM	3	// Unused
#define STAT_REGISTER	4	// Unused
#define STAT_WAITING	5	
#define STAT_PLAYING_GO	6 
#define STAT_SCORING	7
#define STAT_OBSERVING	8	
#define STAT_TEACHING	9	// Unused
#define STAT_COMPLETE	10	// Unused
*/
}
		// BEEP
void IGS_beep::handleMsg(QString line)
{
	if (line.contains("Game saved"))
	{
#ifdef FIXME
		return IT_OTHER;
#endif //FIXME
	}
}

void IGS_down::handleMsg(QString line)
{
	//4 **** Server shutdown started by admin. ****
	/* FIXME Not sure how we want to handle this, but a disconnect
	 * would probably be a good start */
	if(line.contains("Server shutdown"))
	{
		connection->getConsoleDispatch()->recvText("Server shutdown started by admin");
		qDebug("Closing connection in accordance with server shutdown");
		connection->closeConnection();
	}
}
		// ERROR message
		//	5 Player "xxxx" is not open to match requests.
		//	5 You cannot observe a game that you are playing.
		//	5 You cannot undo in this game
		//	5 Opponent's client does not support undoplease
		//	5 noldo is currently involved in a match against someone else.
//	case 5:
void IGS_error::handleMsg(QString line)
{
	static QString memory_str;
	
	RoomDispatch * roomdispatch = connection->getDefaultRoomDispatch();
	qDebug("error? %s", line.toLatin1().constData());
	line = line.remove(0, 2).trimmed();
	if (line.contains("No user named"))
	{
		QString name = element(line, 1, "\"");
//				//emit signal_talk(name, "@@@", true);
	}
	else if (line.contains("is currently involved in a match") || 
		 line.contains("is playing a game") ||
		 line.contains("is involved in another game"))
	{
		QString opp;
		if(line.contains("is playing a game"))	//WING
			opp = element(line, 1, " ", "\"");
		else
			opp = element(line, 0, " ");
		PlayerListing * p = roomdispatch->getPlayerListing(opp);
		GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*p);
		gameDialogDispatch->recvRefuseMatch(GD_REFUSE_INGAME);
	}
	else if (line.contains("is not open to match requests"))
	{
		QString opp = element(line, 0, "\"", "\"");
		PlayerListing * p = roomdispatch->getPlayerListing(opp);
		GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*p);
		gameDialogDispatch->recvRefuseMatch(GD_REFUSE_NOTOPEN);
	}
	else if(line.contains("does not accept direct match"))
	{
		QString opp = element(line, 0, " ");//, " ");
		PlayerListing * p = roomdispatch->getPlayerListing(opp);
		GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*p);
		gameDialogDispatch->recvRefuseMatch(GD_REFUSE_NODIRECT);
	}
	else if (line.contains("player is currently not accepting matches"))
	{
				// IGS: 5 That player is currently not accepting matches.
				//GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(opp);
				//gameDialogDispatch->recvMatchRequest(0, 0);
		qDebug("player not currently accepting matches");
		/* There's no opponent from this message.  A 
		* message window called from the room makes sense.
		* Maybe even like a recvServerAlert function FIXME 
		* I'll just do this for now*/
		if(connection->match_playerName.size())
		{
			PlayerListing * p = roomdispatch->getPlayerListing(connection->match_playerName);
			GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*p);
			gameDialogDispatch->recvRefuseMatch(GD_REFUSE_NOTOPEN);
		}
	}
	else if(line.contains("Invalid parameters"))
	{
		if(connection->match_playerName.size())
		{
			PlayerListing * p = roomdispatch->getPlayerListing(connection->match_playerName);
			GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*p);
			gameDialogDispatch->recvRefuseMatch(GD_INVALID_PARAMETERS);
		}
	}
	//5 Opponent's client does not support nmatch.
	else if(line.contains("does not support nmatch"))
	{
		if(connection->match_playerName.size())
		{
			PlayerListing * p = roomdispatch->getPlayerListing(connection->match_playerName);
			GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*p);
			gameDialogDispatch->recvRefuseMatch(GD_OPP_NO_NMATCH);
		}
	}
	

	else if (line.contains("You cannot undo") || line.contains("client does not support undoplease")) 
	{
				// not the cleanest way : we should send this to a messagez box
#ifdef FIXME
		//emit signal_kibitz(0, 0, line);
#endif //FIXME
	}
	else if(line.contains("There is no such game") || line.contains("Invalid game number"))
	{
		/* This comes from both observe and unobserve messages,
		 * meaning that we can't quite use it to remove gamelistings
		 * unless we fix up the protocol_save_int stuff a little
		 * which needs it anyhow. FIXME */
		//connection->protocol_save_int
		//GameListing * aGame = new GameListing();		
		//RoomDispatch * roomdispatch = connection->getDefaultRoomDispatch();
	}
	else if (line.contains("Setting you open for matches"))
		roomdispatch->recvToggle(0, true);

		// 5 There is a dispute regarding your nmatch:
		// 5 yfh2test request: B 3 19 420 900 25 0 0 0
		// 5 yfh22 request: W 3 19 600 900 25 0 0 0
	//
		// 5 There is a dispute regarding your match:
		// 5   yfh2test wants White on a 19x19 in 10 mins (10 byoyomi).
		// 5   eb5 wants Black on a 19x19 in 10 mins (12 byoyomi).
	

	else if (line.contains("request:"))
	{
		QString p = element(line, 0, " ");
		if (p == connection->getUsername())
		{
			memory_str = line;
			return;
		}
				
		if (memory_str.contains(connection->getUsername() + " request"))
		{
			memory_str = "";
			return;
		}
		
		MatchRequest * aMatch = new MatchRequest();
		aMatch->opponent = p;
				
		if(line.contains("wants"))	//match
		{
			aMatch->nmatch = false;
					
			if(element(line, 2, " ") == "Black")
				aMatch->color_request = MatchRequest::BLACK;
			else if(element(line, 2, " ") == "Nigiri")//IGS handles this??
				aMatch->color_request = MatchRequest::NIGIRI;
			else
				aMatch->color_request = MatchRequest::WHITE;
			QString s = element(line, 5, " ");
			
			aMatch->board_size = element(s, 0, "x").toInt();
			aMatch->maintime = element(line, 7, " ").toInt();
			s = element(line, 9, " ");
			aMatch->periodtime = element(s, 1, "(").toInt();
			aMatch->stones_periods = 25;	// I assume IGS assumes this is always 25
				
		}
		else		//nmatch
		{
			aMatch->nmatch = true;
			if(element(line, 2, " ") == "B")
				aMatch->color_request = MatchRequest::BLACK;
			else if(element(line, 2, " ") == "N")//IGS handles this??
				aMatch->color_request = MatchRequest::NIGIRI;
			else
				aMatch->color_request = MatchRequest::WHITE;
			//N 0 9 120 30 1 3 1 0

			aMatch->handicap = element(line, 3, " ").toInt();
			aMatch->board_size = element(line, 4, " ").toInt();
			aMatch->maintime = element(line, 5, " ").toInt();
			aMatch->periodtime = element(line, 6, " ").toInt();
			aMatch->stones_periods = element(line, 7, " ").toInt();
			if(aMatch->periodtime >= 300)
				aMatch->timeSystem = canadian;
			else
				aMatch->timeSystem = byoyomi;
		}				

				
		PlayerListing * pl = roomdispatch->getPlayerListing(aMatch->opponent);
		PlayerListing * us = roomdispatch->getPlayerListing(connection->getUsername());
		if(us)
		{
			aMatch->our_name = us->name;
			aMatch->our_rank = us->rank;
		}
		if(pl)
			aMatch->their_rank = pl->rank;
		else
		{
			qDebug("No player listing! line: %d", __LINE__);
			delete aMatch;
			return;
		}
		GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*pl);
		gameDialogDispatch->recvRequest(aMatch);
		delete aMatch;		
	}
	else if(line.contains("wants"))
	{
		/* FIXME note that we should really look up their match conditions
		 * before even creating game dialog !!! */
		//5 lemon wants Time 60 - 60.
		//5 seinosuke wants Time 300 - 300
		QString opponent = element(line, 0, " ");
		PlayerListing * pl = roomdispatch->getPlayerListing(opponent);
		if(!pl)
		{
			qDebug("No player listing for %s", opponent.toLatin1().constData());
			return;
		}
		//QString timetochange = element(line, 2, " ");
		GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*pl);
		MatchRequest * m = gameDialogDispatch->getMatchRequest();
		MatchRequest * aMatch = new MatchRequest(*m);
		aMatch->maintime = element(line, 3, " ").toInt();
		aMatch->periodtime = element(line, 4, " ", ".").toInt();
		if(aMatch->periodtime > 299)
			aMatch->timeSystem = canadian;
		else
			aMatch->timeSystem = byoyomi;
		gameDialogDispatch->recvRequest(aMatch);
		gameDialogDispatch->recvRefuseMatch(GD_RESET);
		
	}
	else if(line.contains("is a private game"))
	{
		/* FIXME we need a dialog box saying that its a private game */
		int number = element(line, 2, " ").toInt();
		qDebug("%d is a private game", number);
	}

	connection->getConsoleDispatch()->recvText(line.toLatin1().constData());
}			
		// games
		// 7 [##] white name [ rk ] black name [ rk ] (Move size H Komi BY FR) (###)
		// 7 [41]      xxxx10 [ 1k*] vs.      xxxx12 [ 1k*] (295   19  0  5.5 12  I) (  1)
		// 7 [118]      larske [ 7k*] vs.      T08811 [ 7k*] (208   19  0  5.5 10  I) (  0)
		// 7 [255]          YK [ 7d*] vs.         SOJ [ 7d*] ( 56   19  0  5.5  4  I) ( 18)
		// 7 [42]    TetsuyaK [ 1k*] vs.       ezawa [ 1k*] ( 46   19  0  5.5  8  I) (  0)
		// 7 [237]       s2884 [ 3d*] vs.         csc [ 2d*] (123   19  0  0.5  6  I) (  0)
		// 7 [67]    atsukann [14k*] vs.    mitsuo45 [15k*] ( 99   19  0  0.5 10  I) (  0)
		// 7 [261]      lbeach [ 3k*] vs.    yurisuke [ 3k*] (194   19  0  5.5  3  I) (  0)
		// 7 [29]      ppmmuu [ 1k*] vs.       Natta [ 2k*] (141   19  0  0.5  2  I) (  0)
		// 7 [105]      Clarky [ 2k*] vs.       gaosg [ 2k*] ( 65   19  0  5.5 10  I) (  1)
	//case 7:
void IGS_games::handleMsg(QString line)
{
	if (line.contains("##"))
				// skip first line
		return;
	line = line.remove(0, 2).trimmed();
	QString buffer;
	GameListing * aGame = new GameListing();		
	RoomDispatch * roomdispatch = connection->getDefaultRoomDispatch();		
	// get info line
	buffer = element(line, 0, "(", ")");
	int s = buffer.left(3).toInt();
	buffer.remove(0, 4);
	int number = element(line, 0, "[", "]", true).rightJustified(3).toInt();
			
	GameListing * l = roomdispatch->getGameListing(number);
	if(l)
		*aGame = *l;
	aGame->moves = s;
	aGame->number = number;
	aGame->handicap = element(buffer, 1, " ").toInt();
	aGame->komi = element(buffer, 2, " ").toFloat();
	aGame->board_size = element(buffer, 0, " ").toInt();
	aGame->By = element(buffer, 3, " ").rightJustified(3);
	aGame->FR = element(buffer, 4, " ");

			// parameter "true" -> kill blanks
			
#ifdef FIXME
	gameListB->append(aGame->number);
#endif //FIXME
	aGame->white = roomdispatch->getPlayerListing(element(line, 0, "]", "[", true));
	aGame->_white_name = element(line, 0, "]", "[", true);
	aGame->_white_rank = element(line, 1, "[", "]", true);
	fixRankString(&(aGame->_white_rank));
	aGame->_white_rank_score = connection->rankToScore(aGame->_white_rank);
			// skip 'vs.'
	buffer = element(line, 1, "]", "[", true);
	aGame->black = roomdispatch->getPlayerListing(buffer.remove(0, 3));
	aGame->_black_name = buffer;
	aGame->_black_rank = element(line, 2, "[", "]", true);
	fixRankString(&(aGame->_black_rank));
	aGame->_black_rank_score = connection->rankToScore(aGame->_black_rank);
	aGame->observers = element(line, 1, "(", ")", true).toInt();
			// indicate game to be running
	aGame->running = true;
#ifdef FIXME
	aGame->oneColorGo = false ;
#endif //FIXME

#ifdef FIXME
			//sends signal to main windows (for lists)
	//emit signal_game(aGame);
			//sends signal to interface (for updating game infos)
	//emit signal_gameInfo(aGame);
#endif //FIXME
	roomdispatch->recvGameListing(aGame);
	if(connection->protocol_save_string == "restoring")
	{
		BoardDispatch * boarddispatch = connection->getBoardDispatch(number);
		/* This is ugly, we can get bad info here so we have
		 * to create a half record to send. FIXME*/
		GameData * aGameData = new GameData();
		aGameData->number = aGame->number;
		aGameData->white_name = aGame->white_name();
		aGameData->black_name = aGame->black_name();
		aGameData->white_rank = aGame->white_rank();
		aGameData->black_rank = aGame->black_rank();
		aGameData->board_size = aGame->board_size;
		aGameData->handicap = aGame->handicap;
		aGameData->komi = aGame->komi;
		boarddispatch->recvRecord(aGameData);
		delete aGameData;
		connection->requestGameInfo(number);
		connection->protocol_save_string = QString();
	}
	delete aGame;
}
		// "8 File"
//	case 8:
void IGS_file::handleMsg(QString line)
{
	qDebug("%s", line.toLatin1().constData());
#ifdef FIXME
	if (!memory_str.isEmpty() && memory_str.contains("File"))
	{
				// toggle
		memory_str = QString();
		memory = 0;
	}
	else if (memory != 0 && !memory_str.isEmpty() && memory_str == "CHANNEL")
	{
		//emit signal_channelinfo(memory, line);
		memory_str = QString();
	}

	else if (line.contains("File"))
	{
				// the following lines are help messages
		memory_str = line;
				// check if NNGS message cmd is active -> see '9 Messages:'
		if (memory != 14)
			memory = 8;
	}
#endif //FIXME
}
		// INFO: stats, channelinfo
		// NNGS, LGS: (NNGS new: 2nd line w/o number!)
		//	9 Channel 20 Topic: [xxx] don't pay your NIC bill and only get two players connected
		//	9  xxxx1 xxxx2 xxxx3 xxxx4 frosla
		//	9 Channel 49 Topic: Der deutsche Kanal (German)
		//	9  frosla
		//
		//	-->  channel 49
		//	9 Channel 49 turned on.
		//	9 Channel 49 Title:
		//
		//	9 guest has left channel 49.
		//
		//	9 Komi set to -3.5 in match 10
		// - in my game:
		// -   opponent:
		//      9 Komi is now set to -3.5
		// -   me:
		//      9 Set the komi to -3.5
		// NNGS, LGS:
		//	9 I suggest that ditto play White against made:
		//	9 For 19x19:  Play an even game and set komi to 1.5.
		//	9 For 13x13:  Play an even game and set komi to 6.5.
		//	9 For   9x9:  Play an even game and set komi to 4.5.
		// or:
		//	9 I suggest that pem play Black against eek:
		//	For 19x19:  Take 3 handicap stones and set komi to -2.5.
		//	For 13x13:  Take 1 handicap stones and set komi to -6.5.
		//	For   9x9:  Take 1 handicap stones and set komi to 0.5.
		// or:
		//	   I suggest that you play White against Cut:
		//
		//	9 Match [19x19] in 1 minutes requested with xxxx as White.
		//	9 Use <match xxxx B 19 1 10> or <decline xxxx> to respond.
		//
		//	9 Match [5] with guest17 in 1 accepted.
		//	9 Creating match [5] with guest17.
		//
		//	9 Requesting match in 10 min with frosla as Black.
		//	9 guest17 declines your request for a match.
		//	9 frosla withdraws the match offer.
		//
		//	9 You can check your score with the score command
		//	9 You can check your score with the score command, type 'done' when finished.
		//	9 Removing @ K8
		//
		//	9 Use adjourn to adjourn the game.
		//
		// NNGS: cmd 'user' NOT PARSED!
		//	9  Info     Name       Rank  19  9  Idle Rank Info
		//	9  -------- ---------- ---- --- --- ---- ---------------------------------
		//	9  Q  --  5 hhhsss      2k*   0   0  1s  NR                                    
		//	9     --  5 saturn      2k    0   0 18s  2k                                    
		//	9     --  6 change1     1k*   0   0  7s  NR                                    
		//	9 S   -- -- mikke       1d    0  18 30s  shodan in sweden                      
		//	9   X -- -- ksonney     NR    0   0 56s  NR                                    
		//	9     -- -- kou         6k*   0   0 23s  NR                                    
		//	9 SQ! -- -- GnuGo      11k*   0   0  5m  Estimation based on NNGS rating early 
		//	9   X -- -- Maurice     3k*   0   0 24s  2d at Hamilton Go Club, Canada; 3d in 
void IGS_info::handleMsg(QString line)
{
	static PlayerListing * statsPlayer;
	BoardDispatch * boarddispatch;
	RoomDispatch * roomdispatch = connection->getDefaultRoomDispatch();
	static QString memory_str;
	static int memory = 0;
	qDebug("9: %s", line.toLatin1().constData());
	line = line.remove(0, 2).trimmed();
			// status messages
	if (line.contains("Set open to be"))
	{
		bool val = (line.indexOf("False") == -1);
		roomdispatch->recvToggle(0, val);
	}
	else if (line.contains("Setting you open for matches"))
		roomdispatch->recvToggle(0, true);
	else if (line.contains("Set looking to be"))
	{
		bool val = (line.indexOf("False") == -1);
		roomdispatch->recvToggle(1, val);
	}
			// 9 Set quiet to be False.
	else if (line.contains("Set quiet to be"))
	{
		bool val = (line.indexOf("False") == -1);
		roomdispatch->recvToggle(2, val);
	}
	else if(line.contains("Welcome to the game room"))
	{
		QString buffer = element(line, 6, " ");
		int number = buffer.remove(0,1).toInt();
		qDebug("Joined room number %d", number);
		//FIXME, we need to reload the player
		//and games lists here
	}
	else if (line.indexOf("Channel") == 0) 
	{
#ifdef FIXME
				// channel messages
		QString e1 = element(line, 1, " ");
		if (e1.at(e1.length()-1) == ':')
			e1.truncate(e1.length()-1);
		int nr = e1.toInt();

		if (line.contains("turned on."))
		{
					// turn on channel
			emit signal_channelinfo(nr, QString("*on*"));
		}
		else if (line.contains("turned off."))
		{
					// turn off channel
			emit signal_channelinfo(nr, QString("*off*"));
		}
		else if (!line.contains("Title:") || gsName == GS_UNKNOWN)
		{
					// keep in memory to parse next line correct
			memory = nr;
			emit signal_channelinfo(memory, line);
			memory_str = "CHANNEL";
		}
#endif //FIXME
//				return IT_OTHER;
	}
#ifdef FIXME
	else if (memory != 0 && !memory_str.isEmpty() && memory_str == "CHANNEL")
	{
		//emit signal_channelinfo(memory, line);

				// reset memory
		memory = 0;
		memory_str = QString();
//				return IT_OTHER;
	}
#endif //FIXME
			// IGS: channelinfo
			// 9 #42 Title: Untitled -- Open
			// 9 #42    broesel    zero815     Granit
	else if (line.contains("#"))
	{
#ifdef FIXME
		int nr = element(line, 0, "#", " ").toInt();
		QString msg = element(line, 0, " ", "EOL");
		emit signal_channelinfo(nr, msg);
#endif //FIXME
	}
			// NNGS: channels
	else if (line.contains("has left channel") || line.contains("has joined channel"))
	{
#ifdef FIXME
		QString e1 = element(line, 3, " ", ".");
		int nr = e1.toInt();

				// turn on channel to get full info
		emit signal_channelinfo(nr, QString("*on*"));
#endif //FIXME
	}
	else if (line.contains("Game is titled:"))
	{
#ifdef FIXME
		QString t = element(line, 0, ":", "EOL");
		emit signal_title(t);
#endif //FIXME
		return;
	}
	else if (line.contains("offers a new komi "))
	{
#ifdef FIXME
				// NNGS: 9 physician offers a new komi of 1.5.
		QString komi = element(line, 6, " ");
		if (komi.at(komi.length()-1) == '.')
			komi.truncate(komi.length() - 1);
		QString opponent = element(line, 0, " ");

				// true: request
		emit signal_komi(opponent, komi, true);
#endif //FIXME
	}
	else if (line.contains("Komi set to"))
	{
#ifdef FIXME
				// NNGS: 9 Komi set to -3.5 in match 10
		QString komi = element(line, 3, " ");
		QString game_id = element(line, 6, " ");

				// false: no request
		emit signal_komi(game_id, komi, false);
#endif //FIXME
	}
	else if (line.contains("wants the komi to be"))
	{
#ifdef FIXME
				// IGS: 9 qGoDev wants the komi to be  1.5
		QString komi = element(line, 6, " ");
		QString opponent = element(line, 0, " ");

				// true: request
		emit signal_komi(opponent, komi, true);
#endif //FIXME
	}
	else if (line.contains("Komi is now set to"))
	{
#ifdef FIXME
				// 9 Komi is now set to -3.5. -> oppenent set for our game
		QString komi = element(line, 5, " ");
				// error? "9 Komi is now set to -3.5.9 Komi is now set to -3.5"
		if (komi.contains(".9"))
			komi = komi.left(komi.length() - 2);

				// false: no request
		emit signal_komi(QString(), komi, false);
#endif //FIXME
	}
	else if (line.contains("Set the komi to"))
	{
#ifdef FIXME
				// NNGS: 9 Set the komi to -3.5 - I set for own game
		QString komi = element(line, 4, " ");

				// false: no request
		emit signal_komi(QString(), komi, false);
#endif //FIXME
	}
	else if (line.contains("game will count"))
	{
#ifdef FIXME
				// IGS: 9 Game will not count towards ratings.
				//      9 Game will count towards ratings.
		emit signal_freegame(false);
#endif //FIXME
	}
	else if (line.contains("game will not count", Qt::CaseInsensitive))
	{
#ifdef FIXME
				// IGS: 9 Game will not count towards ratings.
				//      9 Game will count towards ratings.
		emit signal_freegame(true);
#endif //FIXME
	}
	else if ((line.contains("[") || line.contains("yes")) && line.length() < 6)
	{
				// 9 [20] ... channelinfo
				// 9 yes  ... ayt
		return;
	}
	// WING has "as" instead of "has"
	else if (line.contains("as restarted your game") ||
			line.contains("has restored your old game"))
	{
		// FIXME, we probably need to set something up here, though
		// for instance with WING, its a 21 message that really
		// heralds things/info
		if (line.contains("restarted"))
					// memory_str -> see case 15 for continuation
			connection->protocol_save_string = element(line, 0, " ");
	}
	else if (line.contains("I suggest that"))
	{
		memory_str = line;
		return;
	}
	else if (line.contains("and set komi to"))
	{
#ifdef FIXME
		//this might be NNGS, LGS only
				// suggest message ...
		if (!memory_str.isEmpty())
					// something went wrong...
			return;

		line = line.simplified();

		QString p1 = element(memory_str, 3, " ");
		QString p2 = element(memory_str, 6, " ", ":");
		bool p1_play_white = memory_str.contains("play White");

		QString h, k;
		if (line.contains("even game"))
			h = "0";
		else
			h = element(line, 3, " ");

		k = element(line, 9, " ", ".");

		int size = 19;
		if (line.contains("13x13"))
			size = 13;
		else if (line.contains("9x 9"))
		{
			size = 9;
			memory_str = QString();
		}
		if (p1_play_white)
			emit signal_suggest(p1, p2, h, k, size);
		else
			emit signal_suggest(p2, p1, h, k, size);
#endif //FIXME
		return;
	}
			// 9 Match [19x19] in 1 minutes requested with xxxx as White.
			// 9 Use <match xxxx B 19 1 10> or <decline xxxx> to respond.
			// 9 NMatch requested with yfh2test(B 3 19 60 600 25 0 0 0).
			// 9 Use <nmatch yfh2test B 3 19 60 600 25 0 0 0> or <decline yfh2test> to respond.
	else if (line.contains("<decline") && line.contains("match"))
	{
				// false -> not my request: used in mainwin.cpp
				////emit signal_matchRequest(element(line, 0, "<", ">"), false);
		line = element(line, 0, "<", ">");
		MatchRequest * aMatch = new MatchRequest();
		unsigned long flags = 0;
		/* All games are rated except for ones in free room on IGS FIXME */
		aMatch->rated = true;
		aMatch->opponent = line.section(" ", 1, 1);
		if(line.contains("as White"))
			aMatch->color_request = MatchRequest::BLACK;
		else if(line.contains("as Black"))
			aMatch->color_request = MatchRequest::WHITE;
		else if(line.section(" ",2,2) == "B")
			aMatch->color_request = MatchRequest::BLACK;
		else if(line.section(" ",2,2) == "N")
			aMatch->color_request = MatchRequest::NIGIRI;
		else
			aMatch->color_request = MatchRequest::WHITE;
		if(line.contains("nmatch"))
		{
			aMatch->handicap = line.section(" ",3,3).toInt();
			aMatch->board_size = line.section(" ",4,4).toInt();
					//what kind of time? var name?
			//aMatch->maintime = line.section(" ",5,5).toInt();
			//aMatch->byoperiodtime = line.section(" ",6,6).toInt();
			//aMatch->byoperiods = line.section(" " ,7,7).toInt();
			flags = connection->getGameDialogFlags();
			aMatch->maintime = line.section(" ",5,5).toInt();
			aMatch->periodtime = line.section(" ",6,6).toInt();
			aMatch->stones_periods = line.section(" " ,7,7).toInt();
			if(aMatch->periodtime < 300)
				aMatch->timeSystem = byoyomi;
			else
				aMatch->timeSystem = canadian;
			aMatch->nmatch = true;
			qDebug("nmatch in parser");
		}
		else
		{
			aMatch->timeSystem = canadian;
			aMatch->board_size = line.section(" ",3,3).toInt();
			aMatch->maintime = line.section(" ",4,4).toInt() * 60;
			aMatch->periodtime = line.section(" ",5,5).toInt();
			aMatch->stones_periods = 25;
			flags |= GDF_CANADIAN;
			flags |= GDF_STONES25_FIXED;
			aMatch->nmatch = false;
		}
		PlayerListing * p = roomdispatch->getPlayerListing(aMatch->opponent);
		PlayerListing * us = roomdispatch->getPlayerListing(connection->getUsername());
		if(us)
		{	
			aMatch->our_name = us->name;
			aMatch->our_rank = us->rank;
		}
		if(p)
			aMatch->their_rank = p->rank;
		else
		{
			qDebug("No player listing line: %d!", __LINE__);
			delete aMatch;
			return;
		}
		GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*p);
		gameDialogDispatch->recvRequest(aMatch, flags);
		delete aMatch;
	}
			// 9 Match [5] with guest17 in 1 accepted.
			// 9 Creating match [5] with guest17.
	else if (line.contains("Creating match"))
	{
		QString nr = element(line, 0, "[", "]");
				// maybe there's a blank within the brackets: ...[ 5]...
		QString dummy = element(line, 0, "]", ".").trimmed();
		QString opp = element(dummy, 1, " ");

				// We let the 15 game record message create the board
				//GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(opp);
				//gameDialogDispatch->closeAndCreate();
				//GameDialogDispatch * gameDialogDispatch = 
				//		connection->getGameDialogDispatch(opp);
				//MatchRequest * mr = gameDialogDispatch->getMatchRequest();
				//created_match_request = new MatchRequest(*mr);
				//connection->closeGameDialogDispatch(opp);
				////emit signal_matchCreate(nr, opp);
        			// automatic opening of a dialog tab for further conversation
        			////emit signal_talk(opp, "", true);
	}
	else if (line.contains("Match") && line.contains("accepted"))
	{
		QString nr = element(line, 0, "[", "]");
		QString opp = element(line, 3, " ");
				////emit signal_matchCreate(nr, opp);
		/* FIXME, this is probably where we should be doing something? */
				
	}
			// 9 frosla withdraws the match offer.
			// 9 guest17 declines your request for a match.	
	else if (line.contains("declines your request for a match") ||
			line.contains("withdraws the match offer"))
	{
		QString opp = element(line, 0, " ");
		PlayerListing * p = roomdispatch->getPlayerListing(opp);
		GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*p);
		gameDialogDispatch->recvRefuseMatch(1);
	}
			//9 yfh2test declines undo
	else if (line.contains("declines undo"))
	{
				// not the cleanest way : we should send this to a message box
		//emit signal_kibitz(0, element(line, 0, " "), line);
		return;
	}
		
			//9 yfh2test left this room
			//9 yfh2test entered this room
	else if (line.contains("this room"))
	{}	//emit signal_refresh(10);				

			//9 Requesting match in 10 min with frosla as Black.
	else if (line.contains("Requesting match in"))
	{
		QString opp = element(line, 6, " ");
		//emit signal_opponentopen(opp);
	}
			// NNGS: 9 Removing @ K8
			// IGS:	9 Removing @ B5
			//     49 Game 265 qGoDev is removing @ B5
	else if (line.contains("emoving @"))
	{
		/* FIXME DOUBLE CHECK!!! */
				/*if (gsName != IGS)
		{
		QString pt = element(line, 2, " ");
		//emit signal_reStones(pt, 0);
	}*/
		/* 49 handles this for IGS... but what about observed games? */
#ifdef FIXME
		if(connection->protocol_save_int < 0)
		{
			qDebug("Received stone removal message without game in scoring mode");
			return;
		}
		else
		{
			boarddispatch = connection->getBoardDispatch(connection->protocol_save_int);
			GameData * r = connection->getGameData(connection->protocol_save_int);
			if(!r)
			{
				qDebug("Game has no game Record, line: %d", __LINE__);
				return;
			}
			MoveRecord * aMove = new MoveRecord();
			QString pt = element(line, 2, " ");
			aMove->flags = MoveRecord::REMOVE;
			aMove->x = (int)(pt.toAscii().at(0));
			aMove->x -= 'A';
			if(aMove->x < 9)	//no I on IGS
				aMove->x++;
			pt.remove(0,1);
			aMove->y = element(pt, 0, " ").toInt();
			/* Do we need the board size here???*/
			aMove->y = r->board_size + 1 - aMove->y;
			boarddispatch->recvMove(aMove);
			// FIXME Set protocol_save_int to -1???
			delete aMove;
		}
#endif //FIXME
	}
			// 9 You can check your score with the score command, type 'done' when finished.
	else if (line.contains("check your score with the score command"))
	{
//				if (gsName == IGS)
					// IGS: store and wait until game number is known
//					memory_str = QString("rmv@"); // -> continuation see 15
//				else
					////emit signal_restones(0, 0);
					////emit signal_enterScoreMode();
		qDebug("Getting boarddispatch from memory: %d", connection->protocol_save_int);
		boarddispatch = connection->getBoardDispatch(connection->protocol_save_int);
		boarddispatch->recvEnterScoreMode();
		memory_str = QString("rmv@");		
	}
			// IGS: 9 Board is restored to what it was when you started scoring
	else if (line.contains("what it was when you"))
	{
		//emit signal_restoreScore();
	}
			// WING: 9 Use <adjourn> to adjourn, or <decline adjourn> to decline.
	else if (line.contains("Use adjourn to") || line.contains("Use <adjourn> to"))
	{
		qDebug("parser->case 9: Use adjourn to");
				////emit signal_requestDialog("adjourn", "decline adjourn", 0, 0);
		boarddispatch = connection->getBoardDispatch(memory);
		boarddispatch->recvRequestAdjourn();
	}
			// 9 frosla requests to pause the game.
	else if (line.contains("requests to pause"))
	{
		//emit signal_requestDialog("pause", 0, 0, 0);
	}
	else if (line.contains("been adjourned"))
	{
				// re game from list - special case: own game
#ifdef FIXME
		aGame->nr = "@";
		aGame->running = false;

		//emit signal_game(aGame);
#endif //FIXME
		/* There's several after the fact server INFO messages about
		* games adjourning so we only need to close for one of them
		* The 21 message might actually be better FIXME*/
		boarddispatch = connection->getIfBoardDispatch(connection->protocol_save_int);
		if(boarddispatch)
		{
			qDebug("adjourning game!!");
			boarddispatch->adjournGame();
			connection->closeBoardDispatch(connection->protocol_save_int);
		}	
	}
			// 9 Game 22: frosla vs frosla has adjourned.
	else if (line.contains("has adjourned"))
	{
		GameListing * aGame = new GameListing();
				// re game from list
		aGame->number = element(line, 0, " ", ":").toInt();
		aGame->running = false;

				// for information
				//aGame->Sz = "has adjourned.";

#ifdef OLD
		//emit signal_game(aGame);
//				//emit signal_(aGame);
#endif //OLD
				// No need to get existing listing because
				// this is just to falsify the listing
		roomdispatch->recvGameListing(aGame);
		/* Also notify board if we're watching */
		boarddispatch = connection->getIfBoardDispatch(aGame->number);
		if(boarddispatch)
		{
			boarddispatch->adjournGame();
			connection->closeBoardDispatch(aGame->number);
		}
		delete aGame;
	}
			// 9 Removing game 30 from observation list.
	else if (line.contains("from observation list"))
	{
				// is done from qGoIF
				// //emit signal_addToObservationList(-1);
//				aGame->nr = element(line, 2, " ").toInt();
//				aGame->Sz = "-";
//				aGame->running = false;
		//emit signal_observedGameClosed(element(line, 2, " ").toInt());
		return;
	}
			// 9 Adding game to observation list.
	else if (line.contains("to observation list"))
	{
		/* Unfortunately, LGS and IGS have no number here, so
		 * we have to either guess or get it from the observe send
		 * which is easiest */
		connection->getBoardDispatch(connection->protocol_save_int);
		connection->protocol_save_int = -1;
		return;
	}
			// 9 Games currently being observed:  31, 36, 45.
	else if (line.contains("Games currently being observed"))
	{
		if (line.contains("None"))
		{
			//emit signal_addToObservationList(0);
		}
		else
		{
					// don't work correct at IGS!!!
			int i = line.count(',');
			qDebug(QString("observing %1 games").arg(i+1).toLatin1());
//					//emit signal_addToObservationList(i+1);
		}

//				return IT_OTHER;
	}
			// 9 1 minutes were added to your opponents clock
	else if (line.contains("minutes were added"))
	{
#ifdef FIXME
		int t = element(line, 0, " ").toInt();
		emit signal_timeAdded(t, false);
#endif //FIXME
	}
			// 9 Your opponent has added 1 minutes to your clock.
	else if (line.contains("opponent has added"))
	{
#ifdef FIXME
		int t = element(line, 4, " ").toInt();
		emit signal_timeAdded(t, true);
#endif //FIXME
	}
			// NNGS: 9 Game clock paused. Use "unpause" to resume.
	else if (line.contains("Game clock paused"))
	{
#ifdef FIXME
		emit signal_timeAdded(-1, true);
#endif //FIXME
	}
			// NNGS: 9 Game clock resumed.
	else if (line.contains("Game clock resumed"))
	{
#ifdef FIXME
		emit signal_timeAdded(-1, false);
#endif //FIXME
	}
			// 9 Increase frosla's time by 1 minute
	else if (line.contains("s time by"))
	{
#ifdef FIXME
		int t = element(line, 4, " ").toInt();
		if (line.contains(connection->getUsername()))
			emit signal_timeAdded(t, true);
		else
			emit signal_timeAdded(t, false);
#endif //FIXME
	}
			// 9 Setting your . to Banana  [text] (idle: 0 minutes)
	else if (line.contains("Setting your . to"))
	{
		QString busy = element(line, 0, "[", "]");
		if (!busy.isEmpty())
		{
			QString player = element(line, 4, " ");
					// true = player
					////emit signal_talk(player, "[" + busy + "]", true);
		}
	}
	/* I'm pretty sure, as with LGS and WING, we want to
	 * update the game lists here but not use 9 messages
	 * for the board 
	 * 9 message maybe only score with IGS and if we can't
	 * reliably use 22... because of internal difficulties...*/
	else if (line.contains("resigns.")		||
			line.contains("adjourned.")	||
			//don't intefere with status UNLESS IGS (real solution is to
			// interpret status message and ignore here FIXME FIXME)
			line.contains(" : W ", Qt::CaseSensitive)	||
		   	line.contains(" : B ", Qt::CaseSensitive)	||
			line.contains("forfeits on")	||
			line.contains("lost by"))
	{
		GameResult * aGameResult;
		GameListing * aGame = new GameListing();
					// re game from list
		int number = element(line, 0, " ", ":").toInt();
		GameListing * l = roomdispatch->getGameListing(number);
		if(l)
			*aGame = *l;
		aGame->number = number;
		aGame->running = false;
					// for information
		aGame->result = element(line, 4, " ", "}");

		boarddispatch = connection->getBoardDispatch(aGame->number);
		/* FIXME: This shouldn't create a new board if
		* we're not watching it.
		* Also WING sometimes sends 9 and sometimes sends 21 perhaps
		* depending on whether its observed or resign versus score/result
		* we don't want to send double messages.*/
		if(boarddispatch)
		{
			aGameResult = new GameResult();
						
			if(line.contains(" : W ", Qt::CaseSensitive) ||
						line.contains(" : B ", Qt::CaseSensitive))
			{
				aGameResult->result = GameResult::SCORE;

				int posw, posb;
				posw = aGame->result.indexOf("W ");
				posb = aGame->result.indexOf("B ");
				bool wfirst = posw < posb;
				float sc1, sc2;
				sc1 = aGame->result.mid(posw+1, posb-posw-2).toFloat();
					//sc2 = aGame->result.right(aGame->result.length()-posb-1).toFloat();
				/* FIXME This may be right for IGS and wrong for WING */
					//sc2 = aGame->result.mid(posb +1, posb + 6).toFloat();
				sc2 = element(aGame->result, 4, " ").toFloat();
				qDebug("sc1: %f sc2: %f\n", sc1, sc2);
				if(sc1 > sc2)
				{
					aGameResult->winner_score = sc1;
					aGameResult->loser_score = sc2;
				}
				else
				{
					aGameResult->winner_score = sc2;
					aGameResult->loser_score = sc1;

				}

				if (!wfirst)
				{
					int h = posw;
					posw = posb;
					posb = h;
					if(sc2 > sc1)

						aGameResult->winner_color = stoneWhite;
					else
						aGameResult->winner_color = stoneBlack;
				}
				else
				{
					if(sc1 > sc2)
						aGameResult->winner_color = stoneWhite;
					else
						aGameResult->winner_color = stoneBlack;
				}


			}
			else if(line.contains("forfeits on time"))
			{
				aGameResult->result = GameResult::TIME;
				if(line.contains("Black"))
				{
					aGameResult->winner_color = stoneWhite;
				}
				else
				{
					aGameResult->winner_color = stoneBlack;
				}
			}
			else if(line.contains("resign", Qt::CaseInsensitive))
			{
				aGameResult->result = GameResult::RESIGN;
				if(line.contains("Black"))
				{
					aGameResult->winner_color = stoneWhite;
				}
				else
				{
					aGameResult->winner_color = stoneBlack;
				}
			}
			else if(line.contains("lost by"))
			{
				aGameResult->result = GameResult::SCORE;
				if(line.contains("Black"))
				{
					aGameResult->winner_color = stoneWhite;
				}
				else
				{
					aGameResult->winner_color = stoneBlack;

				}

			}
		}


		if (aGame->result.isEmpty())
			aGame->result = "-";
		else if (aGame->result.indexOf(":") != -1)
			aGame->result.remove(0,2);

		if(boarddispatch)
		{
			qDebug("Receiving result!!!");
			boarddispatch->recvResult(aGameResult);
			/* is kibitzing this here what we want?*/
			/* FIXME... should be a shortened list, not the full
			* result msg kibitz */
			boarddispatch->recvKibitz("", line);
		}
		roomdispatch->recvGameListing(aGame);
		delete aGame;
		return;
	}
			// NNGS: 9 Messages: (after 'message' cmd)
			//       8 File
			//       <msg>
			//       8 File
			//       9 Please type "erase" to erase your messages after reading
	else if (line.contains("Messages:"))
	{
				// parse like IGS cmd nr 14: Messages
		memory = 14;
	}
			// IGS:  9 You have a message. To read your messages, type:  message
			// NNGS: 9 You have 1 messages.  Type "messages" to display them
	else if (line.contains("You have") && line.contains("messages"))
	{
				// re cmd nr
		line = line.trimmed();
		line = line.remove(0, 2);
		connection->getConsoleDispatch()->recvText(line.toLatin1().constData());
		return;
	}
			// 9 Observing game  2 (chmeng vs. myao) :
			// 9        shanghai  9k*           henry 15k  
			// 9 Found 2 observers.
	else if (line.contains("Observing game ", Qt::CaseSensitive))
	{
				// right now: only need for observers of teaching game
				// game number
		bool ok;
		memory = element(line, 2, " ").toInt(&ok);
		if (ok)
		{
			memory_str = "observe";
					// FIXME
			//emit signal_clearObservers(memory); 

			return;
		}
	}
	else if (!memory_str.isEmpty() && memory_str == "observe" && line.contains("."))
	{
//				QString cnt = element(line, 1, " ");
//				//emit signal_kibitz(memory, "00", "");

		memory = 0;
		memory_str = QString();

		return;
	}
	else if (!memory_str.isEmpty() && memory_str == "observe")
	{
		QString name =  element(line, 0, " ");
		QString rank;
		boarddispatch = connection->getBoardDispatch(memory);
		if(!boarddispatch)
		{
			qDebug("No boarddispatch for observer list\n");
			return;
		}
				
		for (int i = 1; ! name.isEmpty(); i++)
		{
			rank = element(line, i, " ");
			fixRankString(&rank);
					// send as kibitz from "0"
			PlayerListing * p = roomdispatch->getPlayerListing(name);
			if(p)
				boarddispatch->recvObserver(p, true);
			else
				qDebug("No player listing for %s", name.toLatin1().constData());
			name =  element(line, ++i , " ");
		}

		return;
	}
	else if(line.contains("Found") && line.contains("observers"))
	{
		memory = 0;
		memory_str = QString();
	}
	else if (line.contains("****") && line.contains("Players"))
	{
		
		RoomStats * rs = new RoomStats();
		rs->players = element(line, 1, " ").toInt();
		rs->games = element(line, 3, " ").toInt();
#ifdef OLD
		roomdispatch->recvRoomStats(rs);
#endif //OLD
		qDebug("Room stats: %d %d", rs->players, rs->games);
		delete rs;
				// maybe last line of a 'user' cmd
#ifdef FIXME
		/* I think we can ignore this now. */
		aPlayer->extInfo = "";
		aPlayer->won = "";
		aPlayer->lost = "";
		aPlayer->country = "";
		aPlayer->nmatch_settings = "";
#endif //FIXME
#ifdef FIXME
				/* We might be able to re old
		* games here.  But if this is slow
		* then we shouldn't be using lists like
		* this.  I know there's faster ways
				* to do this. FIXME*/
				/* You know what's particularly bad about
		* this is that it means you have to refresh
		* the players to really refresh the games
		* plus, I'm not really sure its necessary,
		* I just now that there are some bugs in
		* the updates.  We need to experiment/think
		* through it.  Also, I think there's
		* still some issues, listings getting
				* temporarily corrupted FIXME FIXME */
		for(int i = 0; i < gameListB->count(); i++)
		{
			for(int j = 0; j < gameListA->count(); j++)
			{
				if(gameListB->at(i) == gameListA->at(j))
				{
					gameListA->reAt(j);
					break;
				}
			}
		}
		// This is currently really unreliable in addition to being really ugly, all of this here
		for(int i = 0; i < gameListA->count(); i++)
		{
			aGame->number = gameListA->at(i);	
			qDebug("Game id down: %d", aGame->number);
			aGame->running = false;
			roomdispatch->recvGameListing(aGame);
		}

				/* Swap the lists so that the B filled
		* with existing games becomes the A
		* to empty and then delete the next
				* refresh */
		gameListA->clear();
		QList <unsigned int> * list = gameListB;
		gameListB = gameListA;
		gameListA = list;
#endif //FIXME
				// re cmd nr
				//line = line.trimmed();
				//line = line.re(0, 2);
				////emit signal_message(line);
		return;
	}
			// 9 qGoDev has resigned the game.
	else if (line.contains("has resigned the game"))
	{
		if(!connection->protocol_save_int)
		{
			qDebug("no memory for resign game message");
			return;
		}
		/* If this is our game, there's a 21 message maybe later that
		 * should be handling it FIXME */
		boarddispatch = connection->getIfBoardDispatch(connection->protocol_save_int);
		if(!boarddispatch)
		{
			qDebug("No board dispatch for \"resigned the game\"\n");
			return;
		}
#ifdef FIXME
		aGame->running = false;
#endif //FIXME
			
		GameResult * aGameResult = new GameResult();
		aGameResult->loser_name = element(line, 0, " ");
		aGameResult->result = GameResult::RESIGN;
		/* We need to set unknown/none color here */
		boarddispatch->recvResult(aGameResult);
		return;

	}
	else if	(line.contains("has run out of time"))
	{
		boarddispatch = connection->getBoardDispatch(connection->protocol_save_int);
		if(!boarddispatch)
		{
			qDebug("No board dispatch for \"has run out of time\"\n");
			return;
		}
#ifdef FIXME
		aGame->running = false;
#endif //FIXME
			
		GameResult * aGameResult = new GameResult();
		aGameResult->loser_name = element(line, 0, " ");
		aGameResult->result = GameResult::TIME;
		/* We need to set unknown/none color here */
		boarddispatch->recvResult(aGameResult);
		return;

	}



		//9 Player:      yfh2
		//9 Game:        go (1)
		//9 Language:    default
		//9 Rating:      6k*  23
		//9 Rated Games:     21
		//9 Rank:  8k  21
		//9 Wins:        13
		//9 Losses:      16
		//9 Idle Time:  (On server) 0s
		//9 Address:  yfh2@tiscali.fr
		//9 Country:  France
		//9 Reg date: Tue Nov 18 04:01:05 2003
		//9 Info:  yfh2
		//9 Defaults (help defs):  time 0, size 0, byo-yomi time 0, byo-yomi stones 0
		//9 Verbose  Bell  Quiet  Shout  Automail  Open  Looking  Client  Kibitz  Chatter
		//9     Off    On     On     On        On   Off      Off      On      On   On

	else if (line.contains("Player:"))
	{
		//statsPlayer = new PlayerListing();
		QString name = element(line, 1, " ");
		statsPlayer = roomdispatch->getPlayerListing(name);
#ifdef FIXME
				/* So this would have cleared the structure, but
		* we're just creating a new empty object later.
		* One HUGE FIXME question is what we might be
		* overriding on the other side.  Maybe we need
		* a bit vector specifying what has updated when
		* the message is passed.  So the notion before
		* was that the structure was held until
				* completely filled and then passed up*/
				
		statsPlayer->extInfo = "";
		statsPlayer->won = "";
		statsPlayer->lost = "";
		statsPlayer->country = "";
		statsPlayer->nmatch_settings = "";
		statsPlayer->rank = "";
		statsPlayer->info = "";
		statsPlayer->address = "";
		statsPlayer->play_str = "";
		statsPlayer->obs_str = "";
		statsPlayer->idle = "";
		statsPlayer->rated = "";
				// not sure it is the best way : above code seem to make use of "signal"
				// but we don't need this apparently for handling stats
#endif //FIXME
		connection->protocol_save_string = "STATS";
		return;
	}
			
	else if (line.contains("Address:"))
	{
		statsPlayer->email_address = element(line, 1, " ");
		return;
	}
			
	else if (line.contains("Last Access"))
	{
		statsPlayer->idletime = element(line, 4, " ")+ " " + element(line, 5, " ")+" " + element(line, 6, " ");
		statsPlayer->seconds_idle = idleTimeToSeconds(statsPlayer->idletime);
		return;
	}
				
	else if (line.contains("Rating:"))
	{
		statsPlayer->rank = element(line, 1, " ");
		fixRankString(&(statsPlayer->rank));
		statsPlayer->rank_score = connection->rankToScore(statsPlayer->rank);
		return;
	}
			
	else if (line.contains("Wins:"))
	{
		statsPlayer->wins = element(line, 1, " ").toInt();
		return;         
	}
				
	else if (line.contains("Losses:"))
	{
		statsPlayer->losses = element(line, 1, " ").toInt();
		return;  
	}
			
	else if ((line.contains("Country:"))||(line.contains("From:")))   //IGS || LGS
	{
		statsPlayer->country = element(line, 0, " ","EOL");
		return; 
	}
			
	else if (line.contains("Defaults"))    //IGS
	{
		statsPlayer->extInfo = element(line, 2, " ","EOL");
		TalkDispatch * talk = connection->getTalkDispatch(*statsPlayer);
		if(talk)
			talk->updatePlayerListing();
		statsPlayer = 0;
		return; 
	}
	else if (line.contains("User Level:")) //LGS
	{
		statsPlayer->extInfo = line;
		return;
	}
			
				
	else if ((line.contains("Info:"))&& !(line.contains("Rank Info:")))
	{
#ifdef FIXME
		if (! statsPlayer->info.isEmpty())
			statsPlayer->info.append("\n");
		statsPlayer->info.append(line);
		//emit signal_statsPlayer(statsPlayer);
#endif //FIXME
		return;          
	}
			
	else if (line.contains("Playing in game:"))       //IGS and LGS
	{
		statsPlayer->playing = element(line, 3, " ").toInt();

		return;
	}				
	else if (line.contains("Rated Games:"))
	{
		statsPlayer->rated_games = element(line, 2, " ").toInt();
		return;
	}
	else if (line.contains("Idle Time:"))
	{
		statsPlayer->idletime = element(line, 4, " ");
		statsPlayer->seconds_idle = idleTimeToSeconds(statsPlayer->idletime);
		return;
	}
			
	else if (line.contains("Last Access"))
	{
		statsPlayer->idletime = "not on";
		return;
	}
				
	else if (line.left(5) == "19x19")     //LGS syntax
	{
		statsPlayer->rank = element(line, 4, " ");
		fixRankString(&(statsPlayer->rank));
		statsPlayer->rank_score = connection->rankToScore(statsPlayer->rank);
		statsPlayer->rated_games = element(line, 7, " ").toInt();
		return;
	}
			
	else if (line.contains("Wins/Losses"))     //LGS syntax
	{
		statsPlayer->wins = element(line, 2, " ").toInt();
		statsPlayer->losses = element(line, 4, " ").toInt();
		return;
	}
	else if(line.contains("Off") || line.contains("On"))
	{
		/* Scratch this, we're claiming "Defaults" is the last line */
				/*TalkDispatch * talk = connection->getTalkDispatch(statsPlayer->name);
		if(talk)
		talk->recvPlayerListing(statsPlayer);
					//talk->recvTalk(e2);
					
				//roomdispatch->recvExtPlayerListing(statsPlayer);
		delete statsPlayer;
		statsPlayer = 0;*/
	}
				
		//9 ROOM 01: FREE GAME ROOM;PSMNYACF;���R�΋ǎ�;19;1;10;1,19,60,600,30,25,10,60,0
		//9 ROOM 10: SLOW GAME ROOM;PSMNYACF;�۰�΋ǎ�;19;1;20
		//9 ROOM 92: PANDA OPEN ROOM;X;������;19;10;15
	else if (! line.left(5).compare("ROOM "))
	{
		QString room = element(line, 0, " ",";");
		RoomListing * r = new RoomListing(room.section(":", 0, 0).toInt(),
						  room.section(": ", 1, 1));
		r->locked = (element(line, 1,";")=="X") || (element(line, 1,";")=="P");
		connection->recvRoomListing(r);
		//dont delete r
		return;
	}
	else if(line.contains("File"))
		return;
	if (connection->protocol_save_string != "STATS")
		connection->getConsoleDispatch()->recvText(line.toLatin1().constData());
}

// 11 Kibitz Achim [ 3d*]: Game TELRUZU vs Anacci [379]
// 11    will B resign?
//case 11:
void IGS_kibitz::handleMsg(QString line)
{
	static QString memory_str;
	static int memory = 0;
	line = line.remove(0, 2).trimmed();
	BoardDispatch * boarddispatch;
	if (line.contains("Kibitz"))
	{
				// who is kibitzer
		memory_str = element(line, 0, " ", ":");
				// game number
		memory = element(line, 1, "[", "]").toInt();
	}
	else
	{
		if (memory_str.isEmpty())
					// something went wrong...
			return;

		//emit signal_kibitz(memory, memory_str, line);
		boarddispatch = connection->getBoardDispatch(memory);
		if(boarddispatch)
			boarddispatch->recvKibitz(memory_str, line);
		memory = 0;
		memory_str.clear();
	}
}

	// messages
		// 14 File
		// frosla 11/15/02 14:00: Hallo
		// 14 File
void IGS_messages::handleMsg(QString line)
{
	qDebug("%s", line.toLatin1().constData());
#ifdef FIXME	
		//case 14:
	if (!memory_str.isEmpty() && memory_str.contains("File"))
	{
				// toggle
		memory_str = QString();
		memory = 0;
	}
	else if (line.contains("File"))
	{
				// the following lines are help messages
		memory_str = line;
		memory = 14;
	}
#endif //FIXME
};

		// MOVE
		// 15 Game 43 I: xxxx (4 223 16) vs yyyy (5 59 13)
		// 15 TIME:21:lowlow(W): 1 0/60 479/600 14/25 0/0 0/0 0/0
		// 15 TIME:442:MIYASAN(W): 1 0/60 18/30 0/1 10/10 0/60 0/0
		// 15 144(B): B12
		// IGS: teaching game:
		// 15 Game 167 I: qGoDev (0 0 -1) vs qGoDev (0 0 -1)

void IGS_move::handleMsg(QString line)
{
	BoardDispatch * boarddispatch;
	RoomDispatch * roomdispatch = connection->getDefaultRoomDispatch();
		//case 15:
	qDebug("%s", line.toLatin1().constData());
	line = line.remove(0, 2).trimmed();	
	static bool need_time = false;	
	//console->recvText(line.toLatin1().constData());
	/* I think we'll need game_number for scores, so I'm going to
	 * have it on the connection->protocol_saved_int since
	 * hopefully anything that uses it will use it immediately afterward
	 * with no other server msg inbetween */
	//static int game_number = -1;
			//qDebug("Game_number: %d\n", game_number);
	if (line.contains("Game"))
	{
				
		aGameData->number = element(line, 1, " ").toInt();
		aGameData->white_name = element(line, 3, " ");
		aGameData->black_name = element(line, 8, " ");
		
		/* Check if we're reloading the game */
		/* Other problem, with IGS, this might be all we get
		* for a restarted game Is this okay here?  Maybe doesn't
		* always get called?  It could have a listing already.*/
		if(!connection->getIfBoardDispatch(aGameData->number))
		{
		if(aGameData->white_name == connection->getUsername() || aGameData->black_name == connection->getUsername())
		{
			if(connection->protocol_save_string == aGameData->white_name || connection->protocol_save_string == aGameData->black_name)
			{
				qDebug("starting to restore");
					/* Then this is a restarted game, create
					* board dispatch */
					//connection->getBoardDispatch(game_number);
				
				//boarddispatch = connection->getBoardDispatch(aGame->nr);
				//boarddispatch->recvGameListing(aGame);
				connection->requestGameStats(aGameData->number);
				/* Stats will pick up the info and create the board, otherwise
				* boardsize can get lost, etc. */
				connection->protocol_save_string = "restoring";
			}
			else
			{
				/* accept? */
				connection->protocol_save_string = QString();
					/* This code should be unified from that from the other case, msg 9
					* that does this same thing, maybe the 9 should be removed.. FIXME */
				QString opp = (aGameData->white_name == connection->getUsername() ? aGameData->black_name : aGameData->white_name);
				if((aGameData->white_name == connection->getUsername() || aGameData->black_name == connection->getUsername()) && !dynamic_cast<IGSConnection *>(connection)->getBoardFromOurOpponent(opp))
				{
					PlayerListing * p = roomdispatch->getPlayerListing(opp);
					MatchRequest * mr = connection->getAndCloseGameDialogDispatch(*p);
					if(mr)
					{
						qDebug("mr bs: %d", mr->board_size);
						aGameData->board_size = mr->board_size;
						aGameData->komi = mr->komi;
						qDebug("mr komi: %f", aGameData->komi);
						aGameData->handicap = mr->handicap;
						if(aGameData->white_name == connection->getUsername())
						{
							aGameData->white_rank = mr->our_rank;
							aGameData->black_rank = mr->their_rank;
						}
						else
						{
							aGameData->white_rank = mr->their_rank;
							aGameData->black_rank = mr->our_rank;
						}
						boarddispatch = connection->getBoardDispatch(aGameData->number);
						boarddispatch->recvRecord(aGameData);
					}
					else
					{
						PlayerListing * black = roomdispatch->getPlayerListing(aGameData->black_name);
						PlayerListing * white = roomdispatch->getPlayerListing(aGameData->white_name);
						if(black)	
							aGameData->black_rank = black->rank;
						if(white)
							aGameData->white_rank = white->rank;
						// Could be from seek opponent !!!
						boarddispatch = connection->getBoardDispatch(aGameData->number);
						boarddispatch->recvRecord(aGameData);
						need_time = true;
					}
				}
				return;
				/* We probably just accepted a match */
					
			}
				//connection->requestGameInfo(game_number);
				//delete aGame;
		}
		}
		GameListing * l = roomdispatch->getGameListing(aGameData->number);
		if(!l)
		{
			qDebug("Game for unlisted game");
			/* FIXME I'm not sure if we can go without or not */
			/* This creates nasty crash if get this after not
			 * responding to game restart message */
			/* We need board size to create the board or we need
			 * to modify it after which is more ugly.  If a board
			 * is restored, there's generally no listing for it
			 * yet and we have to call something to retrieve the
			 * moves.  WING at least sends the last move here
			 * even before we've had a chance to "refresh"
			 * this can potentially have the ugly affect of
			 * passing that move to another board since we
			 * return here */
			
			connection->protocol_save_int = -1;	//ignore next move
			return;
		}
		else
		{
			aGameData->handicap = l->handicap;
			aGameData->board_size = l->board_size;
			aGameData->komi = l->komi;
		}
		connection->protocol_save_int = aGameData->number;
				//aGameData->type = element(line, 1, " ", ":");
		
		aGameData->white_prisoners = element(line, 0, "(", " ").toInt();
		wtime->time = element(line, 5, " ").toInt();
		wtime->stones_periods = element(line, 5, " ", ")").toInt();
		
		aGameData->black_prisoners = element(line, 1, "(", " ").toInt();
		btime->time = element(line, 10, " ").toInt();
		btime->stones_periods = element(line, 10, " ", ")").toInt();
		/* FIXME the stones can be negative in WING and I'm not sure
		 * what that means */
				/* Is this a new game? 
		* This is this ugly, convoluted way of checking, but I guess
		* it gets around the IGS protocol, maybe I'll think of another way later.
		* We can use the game listing for the s later, but here, there may
		* not be a game listing if its a new game.  This may be the listing, in
				* a sense. */
		/* Another issue here is that we do not want to call this when
		 * restoring a game since there really isn't a gamedialog... FIXME */
	}
	else if (line.contains("TIME"))
	{	
		// FIXME Does WING have these messages???
				// Might not need game record here!!!
#ifdef FIXME
		aGameData->mv_col = "T";
#endif //FIXME
		aGameData->number = element(line, 0, ":",":").toInt();
		connection->protocol_save_int = aGameData->number;
		QString time1 = element(line, 1, " ","/");
		QString time2 = element(line, 2, " ","/");
		QString stones = element(line, 3, " ","/");				

		if(need_time)
		{
			need_time = false;
			if(1)	//FIXME when you see a byoyomi game here
			{
				aGameData->timeSystem = canadian;
				aGameData->maintime = time1.toInt();
				aGameData->periodtime = time2.toInt();
				aGameData->stones_periods = stones.toInt();
			}
			else
			{
				aGameData->timeSystem = byoyomi;
			}
		}
		if (line.contains("(W)"))
		{
			aGameData->white_name = element(line, 1, ":","(");
			wtime->time = (time1.toInt()==0 ? time2 : time1).toInt();
			wtime->stones_periods = (time1.toInt()==0 ?stones: "-1").toInt();
		}
		else if (line.contains("(B)"))					
		{
			aGameData->black_name = element(line, 1, ":","(");
			btime->time = (time1.toInt()==0 ? time2 : time1).toInt();
			btime->stones_periods = (time1.toInt()==0 ? stones:"-1").toInt();
		}
		else //never know with IGS ...
			return;
	}
	else if (line.contains("GAMERPROPS"))
	{
		GameData * gd;
		int game_number = element(line, 0, ":",":").toInt();	
		
		BoardDispatch * boarddispatch = connection->getIfBoardDispatch(game_number);
		if(boarddispatch)
		{
			gd = boarddispatch->getGameData();
			/* We need this for komi in our own games sometimes it seems 
			 * or do we... */
			//GAMERPROPS:86: 9 0 6.50
			gd->board_size = element(line, 1, " ").toInt();
			/* Is that middle one the handicap, FIXME ??? */
			gd->komi = element(line, 3, " ").toFloat();
		}
		return;
	}
	else if(connection->protocol_save_int < 0)
	{
		qDebug("Ignoring boardless move");
		return;
	}
	else
	{
		/* FIXME if two moves are sent on the same line, then
		* I think the last one is real and the one before is
		* an undo, this is when we get a move list initially.
		* I'd rather actually send an undo, so that it can
		* create an undo tree?  But maybe that's unnecessary */
		MoveRecord * aMove = new MoveRecord();
		aMove->flags = MoveRecord::NONE;
		aMove->number = element(line, 0, "(").toInt();
		QString point = element(line, 0, " ", "EOL");
		if(point.contains("Handicap", Qt::CaseInsensitive))
		{
			/* As long as handicap is
			* set in game data... actually
			* this is useful since sending
			*  0 sets tree properly*/
			aMove->flags = MoveRecord::HANDICAP;
			aMove->x = element(point, 1, " ", "EOL").toInt();
			qDebug("handicap %d", aMove->x);
		}
		else if(point.contains("Pass", Qt::CaseInsensitive))
		{
			aMove->flags = MoveRecord::PASS;
		}
		else
		{
			/* If we're getting moves that we can use, then
			 * they have some existing board, which means a boardrecord
			 * which should be more reliable than the listing */
			GameData * r  = connection->getGameData(connection->protocol_save_int);
			//GameListing * l = roomdispatch->getGameListing(connection->protocol_save_int);
			if(!r)
			{
				qDebug("Move for unlisted game");
				delete aMove;
				return;
			}
			qDebug("board size from record: %d", r->board_size);
			
			aMove->x = (int)(point.toAscii().at(0));
			aMove->x -= 'A';
			point.remove(0,1);
			qDebug("move number: %d\n", aMove->number);
			aMove->y = element(point, 0, " ").toInt();
					
			if(aMove->x < 9)	//no I on IGS
				aMove->x++;
			//if(l->board_size > 9)
			//{	
				aMove->y = r->board_size + 1 - aMove->y;
			//}
			qDebug("%d %d\n", aMove->x, aMove->y);
			if(element(line, 0, "(", ")") == "W")
				aMove->color = stoneWhite; 
			else
				aMove->color = stoneBlack; 
		}
		/* Any other color options ??? */
		boarddispatch = connection->getIfBoardDispatch(connection->protocol_save_int);		
		if(boarddispatch)
			boarddispatch->recvMove(aMove);
		delete aMove;
		return;
	}

	/* This also creates the board for first 15 message 
	 * FIXME.  We need to make sure that, if we were to close
	 * a game right before score result was reported, that wouldn't
	 * pop it back up... but maybe 22 deals with that.*/
	
	/* Either this follows an observe, a restore or a gamedialog or it
	 * just pops up randomly and we're expected to ignore it.  */
	boarddispatch = connection->getIfBoardDispatch(aGameData->number);
	if(boarddispatch)
	{
		boarddispatch->recvRecord(aGameData);
		boarddispatch->recvTime(*wtime, *btime);
		if(connection->protocol_save_string == QString("rmv@"))
		{
			boarddispatch->recvEnterScoreMode();
			connection->protocol_save_string = QString();
		}
		/* At the very least for some case 9 messages like
		* resign.  This parser is such a mess but I don't want
		* to write it from scratch, I just want to get it
		* working with the rest of the net code. */
		connection->protocol_save_int = aGameData->number;
		/* This could be set at the wrong time now since we changed the variable FIXME,
		 * see protocol_save_int where it was scoring_game_id */
	}
}
		// SAY
		// 19 *xxxx*: whish you a nice game  :)
		// NNGS - new:
		//  19 --> frosla hallo
void IGS_say::handleMsg(QString line)
{
	BoardDispatch * boarddispatch;
	line = line.remove(0, 2).trimmed();
		//case 19:
//			if (line.contains("-->"))
//				//emit signal_kibitz(0, 0, element(line, 1, " ", "EOL"));
//			else
	boarddispatch = dynamic_cast<class IGSConnection *>(connection)->getBoardFromOurOpponent(element(line, 0, "*", "*"));	
	if(boarddispatch)
		boarddispatch->recvKibitz(element(line, 0, "*", "*"), element(line, 0, ":", "EOL"));
	else
		qDebug("Received kibitz without board for player");
			
}	

/* This is tricky but since 21 reports game status anyway,
 * for observed and played, we don't really need 20. */
void IGS_score_m::handleMsg(QString line)
{
		//20 yfh2 (W:O): 4.5 to NaiWei (B:#): 4.0
		//case 20:
	//qDebug("Ignoring IGS_score_m message");
	//return;
	/* we seem to need this message, especially if its our game */
	GameResult * aGameResult = new GameResult();
	BoardDispatch * boarddispatch;
	bool firstname;
//			aGame->nr = "@";
//			aGame->running = false;
	QString res;
	line = line.remove(0, 2).trimmed();
	QString player = element(line, 0, " ");
	if (player == connection->getUsername())
	{
		player = element(line, 4, " ");
		firstname = 1;
	}
	else
		firstname = 0;
	aGameResult->winner_name = player;
	aGameResult->result = GameResult::SCORE;
	boarddispatch = dynamic_cast<class IGSConnection *>(connection)->getBoardFromOurOpponent(player);
	if(!boarddispatch)
	{
		qDebug("Can't find board for result message!\n");
		return;
	}

	aGameResult->winner_score = element(line, 2, " ").toFloat();
	aGameResult->loser_score = element(line, 6, " ").toFloat();
	if(aGameResult->winner_score > aGameResult->loser_score)
	{
		aGameResult->winner_color = stoneWhite;
		if(firstname)
			aGameResult->winner_name = connection->getUsername();
	}
	else
	{
		float temp = aGameResult->winner_score;
		aGameResult->winner_score = aGameResult->loser_score;
		aGameResult->loser_score = temp;
		aGameResult->winner_color = stoneBlack;
		if(!firstname)
			aGameResult->winner_name = connection->getUsername();
	}
	boarddispatch->recvResult(aGameResult);
	delete aGameResult;
}	
			
			
		// SHOUT - a info from the server
		//case 21:
void IGS_shout::handleMsg(QString line)
{
	PlayerListing * aPlayer;
	
	RoomDispatch * roomdispatch = connection->getDefaultRoomDispatch();
	BoardDispatch * boarddispatch;
	line = line.remove(0, 2).trimmed();
			// case sensitive
	if (line.contains(" connected.}"))
	{
				// {guest1381 [NR ] has connected.}
				//line.replace(QRegExp(" "), "");
		aPlayer = new PlayerListing();

		aPlayer->name = element(line, 0, "{", " ");
		aPlayer->rank = element(line, 0, "[", "]", true);
		fixRankString(&(aPlayer->rank));
		aPlayer->rank_score = connection->rankToScore(aPlayer->rank);
		aPlayer->info = "??";
		aPlayer->playing = -1;
		aPlayer->observing = -1;
		aPlayer->idletime = "-";
		aPlayer->online = true;
				
		roomdispatch->recvPlayerListing(aPlayer);
		delete aPlayer;
			
			
		return;
	}
	else if (line.contains("has disconnected"))
	{
		aPlayer = new PlayerListing();

				// {xxxx has disconnected}
		aPlayer->name = element(line, 0, "{", " ");
		aPlayer->online = false;
				
		roomdispatch->recvPlayerListing(aPlayer);
		delete aPlayer;

		return;
	}
	else if (line.contains("{Game"))
	{
				// {Game 198: xxxx1 vs xxxx2 @ Move 33}
		if (line.contains("@"))
		{
			/* FIXME: This may mean that an old game was just restarted
			 * and so we might want to set it up here */
			/* We also get this message at weird times that makes
			 * boards we're not watching pop up!! FIXME */
			qDebug("Opening up this game... restoring?");
			
			int game_number = element(line, 0, " ", ":").toInt();
			QString white = element(line, 2, " ");
			QString black = element(line, 4, " ");
			
#ifdef OLD
			GameListing * aGame = new GameListing();
					// game has continued
			aGame->nr = element(line, 0, " ", ":");

			aGame->wname = element(line, 2, " ");
			aGame->wrank = "??";
			aGame->bname = element(line, 4, " ");
			aGame->brank = "??";
			aGame->mv = element(line, 6, " ", "}");
			//aGame->Sz = "@";
			//aGame->H = QString();
			aGame->running = true;
#endif //OLD
			if(white == connection->getUsername() || black == connection->getUsername())
			{
				if(connection->protocol_save_string == white || connection->protocol_save_string == black)
				{
					/* Then this is a restarted game, create
					 * board dispatch */
					//connection->getBoardDispatch(game_number);
				
				//boarddispatch = connection->getBoardDispatch(aGame->nr);
				//boarddispatch->recvGameListing(aGame);
				connection->requestGameStats(game_number);
				/* Stats will pick up the info and create the board, otherwise
				 * boardsize can get lost, etc. */
				connection->protocol_save_string = "restoring";
				}
				else
					connection->protocol_save_string = QString();
				//connection->requestGameInfo(game_number);
				//delete aGame;
			}
			return;
		}

				// {Game 155: xxxx vs yyyy has adjourned.}
				// {Game 76: xxxx vs yyyy : W 62.5 B 93.0}
				// {Game 173: xxxx vs yyyy : White forfeits on time.}
				// {Game 116: xxxx17 vs yyyy49 : Black resigns.}
				// IGS:
				// 21 {Game 124: Redmond* vs NaiWei* : Black lost by Resign}
				// 21 {Game 184: Redmond* vs NaiWei* : White lost by 1.0}
                /* This results in double kibitz on LGS, maybe we drop it
		 * altogether? */
		if (line.contains("resigns.")		||
				  line.contains("adjourned.")	||
				  line.contains(" : W ", Qt::CaseSensitive)	||
				  line.contains(" : B ", Qt::CaseSensitive)	||
				  line.contains("forfeits on")	||
				  line.contains("lost by"))
		{
			GameResult * aGameResult;
			GameListing * aGame = new GameListing();
					// re game from list
			int number = element(line, 0, " ", ":").toInt();
			GameListing * l = roomdispatch->getGameListing(number);
			if(l)
				*aGame = *l;
			aGame->number = number;
			aGame->running = false;
					// for information
			aGame->result = element(line, 4, " ", "}");

			boarddispatch = connection->getIfBoardDispatch(aGame->number);
			if(boarddispatch)
			{
				aGameResult = new GameResult();
						
				if(line.contains(" : W ", Qt::CaseSensitive) ||
							       line.contains(" : B ", Qt::CaseSensitive))
				{
					aGameResult->result = GameResult::SCORE;

					int posw, posb;
					posw = aGame->result.indexOf("W ");
					posb = aGame->result.indexOf("B ");
					bool wfirst = posw < posb;
					float sc1, sc2;
					sc1 = aGame->result.mid(posw+1, posb-posw-2).toFloat();
					//sc2 = aGame->result.right(aGame->result.length()-posb-1).toFloat();
					/* FIXME This may be right for IGS and wrong for WING */
					//sc2 = aGame->result.mid(posb +1, posb + 6).toFloat();
					sc2 = element(aGame->result, 4, " ").toFloat();
					qDebug("sc1: %f sc2: %f\n", sc1, sc2);
					if(sc1 > sc2)
					{
						aGameResult->winner_score = sc1;
						aGameResult->loser_score = sc2;
					}
					else
					{
						aGameResult->winner_score = sc2;
						aGameResult->loser_score = sc1;

					}

					if (!wfirst)
					{
						int h = posw;
						posw = posb;
						posb = h;
						if(sc2 > sc1)

							aGameResult->winner_color = stoneWhite;
						else
							aGameResult->winner_color = stoneBlack;
					}
					else
					{
						if(sc1 > sc2)
							aGameResult->winner_color = stoneWhite;
						else
							aGameResult->winner_color = stoneBlack;
					}


				}
				else if(line.contains("forfeits on time"))
				{
					aGameResult->result = GameResult::TIME;
					if(line.contains("Black"))
					{
						aGameResult->winner_color = stoneWhite;
					}
					else
					{
						aGameResult->winner_color = stoneBlack;
					}
				}
				else if(line.contains("resign", Qt::CaseInsensitive))
				{
					aGameResult->result = GameResult::RESIGN;
					if(line.contains("Black"))
					{
						aGameResult->winner_color = stoneWhite;
					}
					else
					{
						aGameResult->winner_color = stoneBlack;
					}
				}
				else if(line.contains("lost by"))
				{
					aGameResult->result = GameResult::SCORE;
					if(line.contains("Black"))
					{
						aGameResult->winner_color = stoneWhite;
					}
					else
					{
						aGameResult->winner_color = stoneBlack;

					}

				}
				else if(line.contains("adjourned"))
				{
					/* A little ugly to have this and return here
					 * but if it works with protocol... */
					boarddispatch->adjournGame();
					connection->closeBoardDispatch(aGame->number);	
					delete aGameResult;
					delete aGame;
					return;
				}
			}


			if (aGame->result.isEmpty())
				aGame->result = "-";
			else if (aGame->result.indexOf(":") != -1)
				aGame->result.remove(0,2);

			if(boarddispatch)
			{
				qDebug("Receiving 21 result!!!");
				/* This can be redundant with 21 */
				boarddispatch->recvResult(aGameResult);
				delete aGameResult; //conditional on boarddispatch!
				/* is kibitzing this here what we want?*/
				boarddispatch->recvKibitz("", line);
			}
			roomdispatch->recvGameListing(aGame);
			delete aGame;
			return;
		}
	}
	else if (line.contains("{Match"))
	{
				// {Match 116: xxxx [19k*] vs. yyyy1 [18k*] }
				// {116:xxxx[19k*]yyyy1[18k*]}
				// WING: {Match 41: o4641 [10k*] vs. Urashima [11k*] H:2 Komi:3.5}
		line.replace(QRegExp("vs. "), "");
		line.replace(QRegExp("Match "), "");
		line.replace(QRegExp(" "), "");
		int number = element(line, 0, "{", ":").toInt();
		GameListing * aGame = new GameListing();
		GameListing * l = roomdispatch->getGameListing(number);
		if(l)
			*aGame = *l;
		aGame->number = number;
		/* No reason to wait for player listing since info is here */
		aGame->white = roomdispatch->getPlayerListing(element(line, 0, ":", "["));
		aGame->_white_name = element(line, 0, ":", "[");
		aGame->_white_rank = element(line, 0, "[", "]");
		fixRankString(&(aGame->_white_rank));
		aGame->_white_rank_score = connection->rankToScore(aGame->_white_rank);
		aGame->black = roomdispatch->getPlayerListing(element(line, 0, "]", "["));
		aGame->_black_name = element(line, 0, "]", "[");
		aGame->_black_rank = element(line, 1, "[", "]");
		fixRankString(&(aGame->_black_rank));
		aGame->_black_rank_score = connection->rankToScore(aGame->_black_rank);
				
		aGame->moves = 0;	//right???
				//aGame->s = "-";
				//aGame->Sz = "-";
				//aGame->handicap = QString();
		aGame->running = true;
				
#ifdef OLD
		if (gsName == WING && aGame->wname == aGame->bname)
					// WING doesn't send 'create match' msg in case of teaching game
			//emit signal_matchCreate(aGame->nr, aGame->bname);

		//emit signal_game(aGame);
#endif //OLD
		roomdispatch->recvGameListing(aGame);
		delete aGame;
		return;
	}
			// !xxxx!: a game anyone ?
	else if (line.contains("!: "))
	{
		QString player = element(line, 0, "!");
		//emit signal_shout(player, line);
		return;
	}
	
	connection->getConsoleDispatch()->recvText(line.toLatin1().constData());
}
void IGS_status::handleMsg(QString line)
{
	qDebug("%s", line.toLatin1().constData());
	line = line.remove(0, 2).trimmed();
		// CURRENT GAME STATUS
		// 22 Pinkie  3d* 21 218 9 T 5.5 0
		// 22 aura  3d* 24 276 16 T 5.5 0
		// 22  0: 4441000555033055001
		// 22  1: 1441100000011000011
		// 22  2: 4141410105013010144
		// 22  3: 1411411100113011114
		// 22  4: 1131111111100001444
		// 22  5: 0100010001005011111
		// 22  6: 0055000101105000010
		// 22  7: 0050011110055555000
		// 22  8: 1005000141005055010
		// 22  9: 1100113114105500011
		// 22 10: 1111411414105010141
		// 22 11: 4144101101100111114
		// 22 12: 1411300103100000144
		// 22 13: 1100005000101001144
		// 22 14: 1050505550101011144
		// 22 15: 0505505001111001444
		// 22 16: 0050050111441011444
		// 22 17: 5550550001410001144
		// 22 18: 5555000111411300144
		//case 22:
			/* Status messages are a bit screwy:
	* the columns are actually the y axis on the board
	* and the rows the x axis, so for instance on our
	* board p19 would appear on their board as 0,14
	* In addition, some things are marked wrong like
	* false eyes that are surrounded by stones are
	* marked as territory when they shouldn't be, but
	* that's not our problem.
	* 0 is a black stone, 1 is a white stone, 3 is
	* dame (no territory (did I get term right?)) 4 is
			* white territory, 5 is black territory */
	static QString player = "";
	static int cap;
	static float komi;
	static BoardDispatch * statusDispatch;
	qDebug(line.toLatin1().constData());
	if (!line.contains(":"))
	{
		if(player == "")
		{
			player = element(line, 0, " ");
			cap = element(line, 2, " ").toInt();
			komi = element(line, 6, " ").toFloat();
		}
#ifdef FIXME
		else if(connection->protocol_save_int > -1)
		{
			MoveRecord * aMove = new MoveRecord();
					/* This could be a huge problem,
			* but we're going to assume that
			* if we get one of these messages
			* and we're scoring a game, it means
					* scoring is done */
			/* FIXME might not be necessary if below attrib handles IF */
			statusDispatch = connection->getIfBoardDispatch(connection->protocol_save_int);
			if(statusDispatch)
			{
				aMove->flags = MoveRecord::DONE_SCORING;
				statusDispatch->recvMove(aMove);
			}
			player = "";
			delete aMove;
		}
#endif //FIXME
		else
		{
			statusDispatch = dynamic_cast<class IGSConnection *>(connection)->getBoardFromAttrib(element(line, 0, " "), element(line, 2, " ").toInt(), element(line, 6, " ").toFloat(), player, cap, komi);
			if(!statusDispatch)
			{
				// FIXME this happens an awful lot.  I think
				// every time we play a game, though perhaps
				// observing games is okay... anyway, either
				// we need to change something so that we don't
				// even check here, or this doesn't warrant
				// a debug message
				qDebug("No status board found!!!\n");
				player = "";
				return;
			}
			statusDispatch->recvEnterScoreMode();
			player = "";
		}
	}
	else
	{
		if(!statusDispatch)
			return;
		int row = element(line, 0, ":").toInt();
		QString results = element(line, 1, " ");
		/* This might be slower than it needs to
		 * be but...  and hardcoding board,
		 * I'd like to fix this, but it would require... 
		 * like a MoveRecord->next kind of pointer so that
		 * we could send whole lists at once.  But is that
		 * worth it?  Seems like an ultimately useless
		 * functionality. */
		/* X and Y are swapped below */
		MoveRecord * aMove = new MoveRecord();
		aMove->x = row + 1;		
		aMove->flags = MoveRecord::TERRITORY;
		for(unsigned int column = 1; column <= strlen(results.toLatin1().constData()); column++) 
		{
			aMove->y = column;
			if(results.at(column - 1) == '4')
				aMove->color = stoneWhite;
			else if(results.at(column - 1) == '5')
				aMove->color = stoneBlack;
			else
				continue;
			statusDispatch->recvMove(aMove);
		}
		
		/* FIXME If there's no more moves, and there's no result
		 * on the game (like an earlier resign at the same time? etc.,
		 * before it?  Then we need to recvResult here */
		if(row + 1 == (int)strlen(results.toLatin1().constData()))
		{
			/* Since its a square board */
			aMove->flags = MoveRecord::DONE_SCORING;
			statusDispatch->recvMove(aMove);
		}
		delete aMove;
	}

//			//emit signal_message(line);
}
void IGS_stored::handleMsg(QString line)
{
	qDebug("%s", line.toLatin1().constData());
		// STORED
		// 9 Stored games for frosla:
		// 23           frosla-physician
//TODO		case 23:
//TODO			break;
}
void IGS_tell::handleMsg(QString line)
{
	line = line.remove(0, 2).trimmed();
		//  24 *xxxx*: CLIENT: <cgoban 1.9.12> match xxxx wants handicap 0, komi 5.5, free
		//  24 *jirong*: CLIENT: <cgoban 1.9.12> match jirong wants handicap 0, komi 0.5
    		//  24 *SYSTEM*: shoei canceled the nmatch request.
		//  24 *SYSTEM*: xxx requests undo.

		// NNGS - new version:
		//  24 --> frosla CLIENT: <qGo 0.0.15b7> match frosla wants handicap 0, komi 0.5, free
		//  24 --> frosla  Hallo
		//case 24:
		//{
	qDebug("24: %s", line.toLatin1().constData());
	int pos;
	BoardDispatch * boarddispatch;
	RoomDispatch * roomdispatch = connection->getDefaultRoomDispatch();
	if ((((pos = line.indexOf("*")) != -1) && (pos < 3) || 
		      ((pos = line.indexOf("-->")) != -1) && (pos < 3)) &&
		      line.contains("CLIENT:"))
	{
#ifdef FIXME
		line = line.simplified();
		QString opp = element(line, 1, "*");
		int offset = 0;
		if (opp.isEmpty())
		{
			offset++;
			opp = element(line, 1, " ");
		}

		QString h = element(line, 7+offset, " ", ",");
		QString k = element(line, 10+offset, " ");

		if (k.at(k.length()-1) == ',')
			k.truncate(k.length() - 1);
		int komi = (int) (k.toFloat() * 10);

		bool free;
		if (line.contains("free"))
			free = true;
		else
			free = false;

		emit signal_komirequest(opp, h.toInt(), komi, free);
		connection->getConsoleDispatch()->recvText(line.toLatin1().constData());

		// it's tell, but don't open a window for that kind of message...
#endif //FIXME
		return;
	}
      
      			//check for cancelled game offer
	if (line.contains("*SYSTEM*"))
	{
		QString opp = element(line, 1, " ");
		PlayerListing * p = roomdispatch->getPlayerListing(opp);
		line = line.remove(0,10);
		connection->getConsoleDispatch()->recvText(line.toLatin1().constData());

		if  (line.contains("canceled") &&  line.contains("match request"))
		{	
					/* FIXME We need to close any open gamedialog boxes
			* pertaining to this opponent.  We'll assume
					* one is created I guess */	
			
			GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*p);
			gameDialogDispatch->recvRefuseMatch(2);
					////emit signal_matchCanceled(opp);
		}
		if  (line.contains("requests undo"))
		{
			boarddispatch = connection->getBoardDispatch(connection->protocol_save_int);
			if(!boarddispatch)
			{
				printf("No boarddispatch for undo message");
				return;
			}
			MoveRecord * aMove = new MoveRecord();
			aMove->flags = MoveRecord::REQUESTUNDO;
			boarddispatch->recvMove(aMove);
			delete aMove;
					////emit signal_requestDialog("undo","noundo",0,opp);
		}
		return;
	}
      
			// check for NNGS type of msg
	QString e1,e2;
	if ((pos = line.indexOf("-->")) != -1 && pos < 3)
	{
		e1 = element(line, 1, " ");
				//e2 = "> " + element(line, 1, " ", "EOL").trimmed();
		e2 = element(line, 1, " ", "EOL").trimmed();
	}
	else
	{
		e1 = element(line, 0, "*", "*");
				//e2 = "> " + element(line, 0, ":", "EOL").trimmed();
		e2 = element(line, 0, ":", "EOL").trimmed();
	}

			// //emit player + message + true (=player)
			////emit signal_talk(e1, e2, true);
	PlayerListing * p = roomdispatch->getPlayerListing(e1);
	TalkDispatch * talk = connection->getTalkDispatch(*p);
	if(talk)
		talk->recvTalk(e2);
			
}


		// results
		//25 File
		//curio      [ 5d*](W) : lllgolll   [ 4d*](B) H 0 K  0.5 19x19 W+Resign 22-04-47 R
		//curio      [ 5d*](W) : was        [ 4d*](B) H 0 K  0.5 19x19 W+Time 22-05-06 R
		//25 File
		//case 25:
void IGS_thist::handleMsg(QString line)		
{
	//FIXME if this is something that happens then
	// we should be picking it up
	qDebug("%s", line.toLatin1().constData());
}
		// who
		// 27  Info Name Idle Rank | Info Name Idle Rank
		// 27  SX --   -- xxxx03      1m     NR  |  Q! --   -- xxxx101    33s     NR  
		// 0   4 6    11  15         26     33   38 41
		// 27     --  221 DAISUKEY   33s     8k  |   X172   -- hiyocco     2m    19k*
		// 27  Q  --  216 Saiden      7s     1k* |     --   53 toshiao    11s    10k 
		// 27     48   -- kyouji      3m    11k* |     --   95 bengi       5s     4d*
		// IGS:
		//        --   -- kimisa      1m     2k* |  Q  --  206 takabo     45s     1k*
		//      X 39   -- Marin       5m     2k* |     --   53 KT713      18s     2d*
		//        --   34 mat21       2m    14k* |     --    9 entropy    28s     4d 
		// NNGS:
		// 27   X --   -- arndt      21s     5k  |   X --   -- biogeek    58s    20k   
		// 27   X --   -- buffel     21s     4k  |  S  --    5 frosla     12s     7k   
		// 27  S  --   -- GoBot       1m     NR  |     --    5 guest17     0s     NR   
		// 27     --    3 hama        3s    15k  |     --   -- niki        5m     NR   
		// 27   ! --   -- viking4    55s    19k* |  S  --    3 GnuGo       6s    14k*  
		// 27   X --   -- kossa      21m     5k  |   X --   -- leif        5m     3k*  
		// 27     --    6 ppp        18s     2k* |     --    6 chmeng     20s     1d*  
		// 27                 ********  14 Players 3 Total Games ********

		// WING:
		// 0        9    14                      38      46   51
		// 27   X 13   -- takeo6      1m     2k  |   ! 26   -- ooide       6m     2k*  
		// 27   ! --   -- hide1234    9m     2k* |  QX --   -- yochi       0s     2k*  
		// 27   g --   86 karasu     45s     2k* |  Sg --   23 denakechi  16s     1k*  
		// 27   g 23   43 kH03       11s     1k* |   g --   43 kazusige   24s     1k*  
		// 27   g --   50 jan        24s     1k* |  QX --   -- maigo      32s     1d   
		// 27   g --   105 kume1       5s     1d* |   g --   30 yasumitu   24s     1d   
		// 27  Qf --   13 motono      2m     1d* |   X 13   -- tak7       57s     1d*  
		// 27   ! 50   -- okiek       8m     1d* |   X --   -- DrO        11s     1d*  
		// 27   f --   103 hiratake   35s     8k  |   g --   33 kushinn    21s     8k*
		// 27   g --   102 teacup      1m     1d* |   g --   102 Tadao      32s     1d*  
		//case 27:
void IGS_who::handleMsg(QString line)
{
	PlayerListing * aPlayer;
	RoomDispatch * roomdispatch = connection->getDefaultRoomDispatch();
	int pos;
	//line = line.remove(0, 2).trimmed();
			// search for first line
	if (line.contains("Idle") && (line.indexOf("Info")) != -1)
	{
				// skip
		//return PLAYER27_START;
	}
	else if (line.contains("****") && line.contains("Players"))
	{
		
		RoomStats * rs = new RoomStats();
		rs->players = element(line, 2, " ").toInt();
		rs->games = element(line, 4, " ").toInt();
#ifdef OLD
		roomdispatch->recvRoomStats(rs);
#endif //OLD
		qDebug("Room stats: %d %d", rs->players, rs->games);
		delete rs;
	}

	aPlayer = new PlayerListing();
			// indicate player to be online
	aPlayer->online = true;
			
			/* This chunk is such a repetitive ugly mess... I mean I guess this
	* whole text parser is, almost by necessity, but still.  I wonder if
			* it couldn't be cleaned up.  FIXME */
#ifdef FIXME
	if (gsName == WING)
	{
				// shifts take care of too long integers
		int shift1 = (line[9] == ' ' ? 0 : 1);
		int shift2 = (line[14+shift1] == ' ' ? shift1 : shift1+1);
		int shift3 = (line[46+shift2] == ' ' ? shift2 : shift2+1);
		int shift4 = (line[51+shift3] == ' ' ? shift3 : shift3+1);

		if (line[15+shift2] != ' ')
		{
					// parse line
			aPlayer->info = line.mid(4,2);
			aPlayer->observing = line.mid(7,3).toInt();
			aPlayer->playing = line.mid(12+shift1,3).trimmed().toInt();
			aPlayer->name = line.mid(15+shift2,11).trimmed();
			aPlayer->idletime = line.mid(26+shift2,3);
			aPlayer->seconds_idle = idleTimeToSeconds(aPlayer->idletime);
			if (line[33+shift2] == ' ')
			{
				if (line[36] == ' ')
					aPlayer->rank = line.mid(34+shift2,2);
				else
					aPlayer->rank = line.mid(34+shift2,3);
				fixRankString(&(aPlayer->rank));
				aPlayer->rank_score = rankToScore(aPlayer->rank);
			}
			else
			{
				if (line[36+shift2] == ' ')
					aPlayer->rank = line.mid(33+shift2,3);
				else
					aPlayer->rank = line.mid(33+shift2,4);
				fixRankString(&(aPlayer->rank));
				aPlayer->rank_score = rankToScore(aPlayer->rank);
			}
					
					// check if line ok, true -> cmd "players" preceded
			roomdispatch->recvPlayerListing(aPlayer);
		}
		else
			qDebug("WING - player27 dropped (1): %s" , line.toLatin1().constData());

				// position of delimiter between two players
		pos = line.indexOf('|');
				// check if 2nd player in a line && player (name) exists
		if (pos != -1 && line[52+shift4] != ' ')
		{
					// parse line
			aPlayer->info = line.mid(41+shift2,2);
			aPlayer->observing = line.mid(44+shift2,3).toInt();
			aPlayer->playing = line.mid(49+shift3,3).trimmed().toInt();
			aPlayer->name = line.mid(52+shift4,11).trimmed();
			aPlayer->idletime = line.mid(63+shift4,3);
			aPlayer->seconds_idle = idleTimeToSeconds(aPlayer->idletime);
			if (line[70+shift4] == ' ')
			{
				if (line[73+shift4] == ' ')
					aPlayer->rank = line.mid(71+shift4,2);
				else
					aPlayer->rank = line.mid(71+shift4,3);
				fixRankString(&(aPlayer->rank));
				aPlayer->rank_score = rankToScore(aPlayer->rank);
			}
			else
			{
				if (line[73+shift4] == ' ')
					aPlayer->rank = line.mid(70+shift4,3);
				else
					aPlayer->rank = line.mid(70+shift4,4);
				fixRankString(&(aPlayer->rank));
				aPlayer->rank_score = rankToScore(aPlayer->rank);
			}

					// true -> cmd "players" preceded
			roomdispatch->recvPlayerListing(aPlayer);
		}
		else
			qDebug("WING - player27 dropped (2): %s" + line.toLatin1());
	}
	else
	{
#endif //FIXME
		if (line[15] != ' ')
		{
//27   X --   -- truetest    7m     1d* |   X --   -- aajjoo      6s      9k
					// parse line
			aPlayer->info = line.mid(4,2);
			aPlayer->observing = line.mid(6,3).toInt();
			aPlayer->playing = line.mid(11,3).trimmed().toInt();
			aPlayer->name = line.mid(15,11).trimmed();
			aPlayer->idletime = line.mid(26,3);
			aPlayer->seconds_idle = idleTimeToSeconds(aPlayer->idletime);
					//aPlayer->nmatch = false;
			if (line[33] == ' ')
			{
				if (line[36] == ' ')
					aPlayer->rank = line.mid(34,2);
				else
					aPlayer->rank = line.mid(34,3);
				fixRankString(&(aPlayer->rank));
				aPlayer->rank_score = connection->rankToScore(aPlayer->rank);
			}
			else
			{
				if (line[36] == ' ')
					aPlayer->rank = line.mid(33,3);
				else
					aPlayer->rank = line.mid(33,4);
				fixRankString(&(aPlayer->rank));
				aPlayer->rank_score = connection->rankToScore(aPlayer->rank);
			}
					
					// check if line ok, true -> cmd "players" preceded
			roomdispatch->recvPlayerListing(aPlayer);
		}
		else
			qDebug("player27 dropped (1): %s" + line.toLatin1());

				// position of delimiter between two players
		pos = line.indexOf('|');
				// check if 2nd player in a line && player (name) exists
		if (pos != -1 && line[52] != ' ')
		{
					// parse line
			aPlayer->info = line.mid(41,2);
			aPlayer->observing = line.mid(43,3).toInt();
			aPlayer->playing = line.mid(48,3).trimmed().toInt();
			aPlayer->name = line.mid(52,11).trimmed();
			aPlayer->idletime = line.mid(63,3);
			aPlayer->seconds_idle = idleTimeToSeconds(aPlayer->idletime);
					//aPlayer->nmatch = false;
			if (line[70] == ' ')
			{
				if (line[73] == ' ')
					aPlayer->rank = line.mid(71,2);
				else
					aPlayer->rank = line.mid(71,3);
				fixRankString(&(aPlayer->rank));
				aPlayer->rank_score = connection->rankToScore(aPlayer->rank);
			}
			else
			{
				if (line[73] == ' ')
					aPlayer->rank = line.mid(70,3);
				else
					aPlayer->rank = line.mid(70,4);
				fixRankString(&(aPlayer->rank));
				aPlayer->rank_score = connection->rankToScore(aPlayer->rank);
			}

					// true -> cmd "players" preceded
			roomdispatch->recvPlayerListing(aPlayer);
		}
		else
			qDebug("player27 dropped (2): %s" + line.toLatin1());
	
	delete aPlayer;
}

void IGS_undo::handleMsg(QString line)
		// 28 guest17 undid the last  (J16).
		// 15 Game 7 I: frosla (0 5363 -1) vs guest17 (0 5393 -1)
		// 15   2(B): F17
		// IGS:
		// 28 Undo in game 64: MyMaster vs MyMaster:  N15 
		//case 28:
{
	BoardDispatch * boarddispatch;
	qDebug("*** %s", line.toLatin1().constData());
	line = line.remove(0, 2).trimmed();
	if (line.contains("undid the last "))
	{
				// now: just look in qgo_interface if _nr has decreased...
				// but send undo-signal anyway: in case of undo while scoring it's necessary
		MoveRecord * aMove = new MoveRecord();
		aMove->flags = MoveRecord::UNDO;
		QString player = element(line, 0, " ");
		QString point = element(line, 0, "(", ")");

		aMove->x = (int)(point.toAscii().at(0));
		aMove->x -= 'A';
		if(aMove->x < 9)	//no I on IGS
			aMove->x++;
		point.remove(0,1);
		aMove->y = element(point, 0, " ").toInt();
		/* Do we need the board size here???*/
		aMove->y = 20 - aMove->y;

		boarddispatch = dynamic_cast<class IGSConnection *>(connection)->getBoardFromOurOpponent(player);
		boarddispatch->recvMove(aMove);
		delete aMove;
	}
	else if (line.contains("Undo in game"))
	{
		MoveRecord * aMove = new MoveRecord();
		aMove->flags = MoveRecord::UNDO;
		QString nr = element(line, 3, " ");
		nr.truncate(nr.length() - 1);
		QString point = element(line, 7, " ");
		aMove->x = (int)(point.toAscii().at(0));
		aMove->x -= 'A';
		if(aMove->x < 9)	//no I on IGS
			aMove->x++;
		point.remove(0,1);
		aMove->y = element(point, 0, " ").toInt();
		/* Do we need the board size here???*/
		aMove->y = 20 - aMove->y;
		boarddispatch = connection->getBoardDispatch(nr.toInt());		
		boarddispatch->recvMove(aMove);
		delete aMove;
	}

			// message anyway
	connection->getConsoleDispatch()->recvText(line.toLatin1().constData());
}

void IGS_yell::handleMsg(QString line)
		// different from shout??!?!?
		// IGS
		// -->  ; \20
		// 32 Changing into channel 20.
		// 32 Welcome to cyberspace.
		//
		// 32 49:qgodev: Title is now: (qgodev) qGo development
		// 32 20:qGoDev: Person joining channel
		// 32 20:frosla: hi
		// 32 20:qGoDev: Person leaving channel

		//
		// 1 5
		// -->  channels
		// 9 #1 Title: Professional Channel -- Open
		// 
		// 9 #20 Title: Untitled -- Open
		// 9 #20     frosla
		// #> 23 till: hello all  <-- this is what the members in channel 23 see:
		//			(channel number and who sent the message)
		//
		// NNGS:
		// channel talk: "49:xxxx: hi"
		//case 32:
{
	/* As far as I can tell, the channel info stuff was
	 * never hooked up to anything.  The talk should be
	 * hooked up to something, but this is a FIXME*/
	QString e1,e2;
	qDebug("%s\n", line.toLatin1().constData());
	line = line.remove(0, 2).trimmed();
	if (line.contains("Changing into channel"))
	{
#ifdef FIXME
		e1 = element(line, 2, " ",".");
		int nr = e1.toInt();//element(line, 3, " ").toInt();
		emit signal_channelinfo(nr, QString("*on*"));
//				//emit signal_talk(e1, "", false);
				////emit signal_message(line);
#endif //FIXME
		return;
	}
	else if (line.contains("Welcome to cyberspace"))
	{
				////emit signal_message(line);
		connection->getConsoleDispatch()->recvText(line.toLatin1().constData());
		return;
	}
	else if (line.contains("Person joining channel"))
	{
#ifdef FIXME
		int nr = element(line, 0, ":").toInt();
		emit signal_channelinfo(nr, QString("*on*"));
#endif //FIXME
		return;
	}
	else if (line.contains("Person leaving channel"))
	{
#ifdef FIXME
		int nr = element(line, 0, ":").toInt();
		emit signal_channelinfo(nr, QString("*on*"));
#endif //FIXME
		return;
	}

			// //emit (channel number, player + message , false =channel )
			
#ifdef FIXME
	switch (gsName)
	{
		case IGS:
			e1=element(line, 0, ":");
			e2="> " + element(line, 0, ":", "EOL").trimmed();
			break;

		default:
			e1=element(line, 0, ":");
			e2="> " + element(line, 0, ":", "EOL").trimmed();
			break;
	}
#endif //FIXME

//			//emit signal_talk(e1, e2, false);
}


/* 36 
36 terra1 wants a match with you:
36 terra1 wants 19x19 in 1 minutes with 10 byo-yomi and 25 byo-stones
36 To accept match type 'automatch terra1'
*/	
void IGS_automatch::handleMsg(QString line)
{
	line = line.remove(0, 2).trimmed();
	if(line.contains("minutes"))
	{
		RoomDispatch * roomdispatch = connection->getDefaultRoomDispatch(); 
		MatchRequest * aMatch = new MatchRequest();
		aMatch->opponent = element(line, 0, " ");
		
		qDebug("Receieved automatch request from %s", aMatch->opponent.toLatin1().constData());
		QString bs = element(line, 2, " ");
		if(bs == "19x19")
			aMatch->board_size = 19;
		else if(bs == "13x13")
			aMatch->board_size = 13;
		else if(bs == "9x9")
			aMatch->board_size = 9;
		else
		{
			qDebug("Match offered board size: %s", bs.toLatin1().constData());
			delete aMatch;
			return;
		}
		aMatch->maintime = element(line, 4, " ").toInt();
		aMatch->periodtime = element(line, 7, " ").toInt();
		aMatch->stones_periods = element(line, 10, " ").toInt();
		PlayerListing * p = roomdispatch->getPlayerListing(aMatch->opponent);
		PlayerListing * us = roomdispatch->getPlayerListing(connection->getUsername());
		if(us)
		{	
			aMatch->our_name = us->name;
			aMatch->our_rank = us->rank;
		}
		if(p)
			aMatch->their_rank = p->rank;
		/* Maybe we have to do automatch, rather than accept/decline,
		 * FIXME */
		//connection->protocol_save_string = "automatch";
		GameDialogDispatch * gameDialogDispatch = connection->getGameDialogDispatch(*p);
		gameDialogDispatch->recvRequest(aMatch);
		
		delete aMatch;
	}
}

void IGS_serverinfo::handleMsg(QString)
{}

		// Setting your . to xxxx
		//case 40:
void IGS_dot::handleMsg(QString)
{}
		// long user report equal to 7
		// 7 [255]          YK [ 7d*] vs.         SOJ [ 7d*] ( 56   19  0  5.5  4  I) ( 18)
		// 42 [88]          AQ [ 2d*] vs.        lang [ 2d*] (177   19  0  5.5  6  I) (  1)

		// IGS: cmd 'user'
		// 42 Name        Info            Country  Rank Won/Lost Obs  Pl Idle Flags Language
		// 0  3           15              31       40   45 48    54   59 62   67    72
		// 42    guest15                  --        NR    0/   0  -   -   10s    Q- default 
		// 42    guest13                  --        NR    0/   0  -   -   24s    Q- default 
		// 42         zz  You may be rec  France    NR    0/   0  -   -    0s    -X default 
		// 42     zz0002  <none>          Japan     NR    0/   0  -   -    1m    QX default 
		// 42     zz0003  <none> ()       Japan     NR    0/   0  -   -    1m    SX default 
		// 42       anko                  Taiwan    2k* 168/  97  -   -    1s    -- default 
		// 42     MUKO12  1/12 �` 1/15    Japan     4k* 666/ 372  17  -    1m    -X default 
		// 42        mof  [Igc2000 1.1]   Sweden    2k* 509/ 463 124  -    0s    -X default 
		// 42     obiyan  <None>          Japan     3k* 1018/ 850  -   50  11s    -- default 
		// 42    uzumaki                  COM       1k    0/   0  -   -    1s    -X default 
		// 42 HansWerner  [Igc2000 1.2]   Germany   1k   11/   3 111  -    1m    QX default 
		// 42    genesis                  Germany   1k* 592/ 409  -  136  14s    -- default 
		// 42    hamburg                  Germany   3k* 279/ 259  -  334  13s    -- default 
		// 42        Sie                  Germany   1k*   6/   7  -   68  39s    -- default 
		// 42     borkum                  Germany   4k* 163/ 228  -  100   7s    -- default 
		// 42     casumy  45-3            Germany   1d    0/   0  -  133   2s    Q- default 
		// 42      stoni                  Germany   2k* 482/ 524  -  166   7s    -- default 
		// 42   Beholder  <None>          Germany   NR    0/   0 263  -    5s    QX default 
		// 42     xiejun  1/10-15         Germany   3d* 485/ 414 179  -   49s    -X default
		// 9                 ******** 1120 Players 247 Total Games ********

		// IGS: new cmd 'userlist' for nmatch information
		//          10        20        30        40        50        60        70        80        80        90       100       110       120
		// 01234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789
		// 42 Name        Info            Country  Rank Won/Lost Obs  Pl Idle Flags Language
		// 42      -----  <none>          Japan    15k+ 2108/2455  -   -    1m    Q- default  T BWN 0-9 19-19 600-600 1200-1200 25-25 0-0 0-0 0-0
		// 42      -----           Japan     3d   11/  13  -  222  46s    Q- default  T BWN 0-9 19-19 60-60 600-600 25-25 0-0 0-0 0-0
		// 42       Neil  <None>          USA      12k  136/  86  -   -    0s    -X default  T BWN 0-9 19-19 60-60 60-3600 25-25 0-0 0-0 0-0
		//case 42:
		
		
//42    koumas*                  -        11k  0000/0000  -    4  11m    9a E?E----

//		42    koumas*                  -        11k  0000/0000  -    4  11m    9a E?E----
//		No match



//		42    spaman*                  -        11k* 0000/0000  -    4  11m    9a E?E----

//		42    spaman*                  -        11k* 0000/0000  -    4  11m    9a E?E----
//		No match



void IGS_userlist::handleMsg(QString line)
{
	//line = line.remove(0, 2).trimmed();
	PlayerListing * aPlayer;
	RoomDispatch * roomdispatch = connection->getDefaultRoomDispatch();
	if (line[40] == 'R')	//this was line ??? 
	{
				// skip
		return;
	}
			//                        1                   
			//                        Name
	QRegExp re1 = QRegExp("42 ([A-Za-z0-9 ]{,10}) ");
			//			2
			//			Info
	QRegExp re2 = QRegExp("(.{1,14})  ");
			//                    3
			//                    Country
	QRegExp re3 = QRegExp("([a-zA-Z][a-zA-Z. /]{,6}|--     )  "
			//                    4
			//                    Rank
			"([0-9 ][0-9][kdp].?| +BC| +NR) +"
			//                    5               6
			//                    Won             Lost
			"([0-9 ]+[0-9]+)/([0-9 ]+[0-9]+) +"
			//                    7           8
			//                    Obs         Pl
			"([0-9]+|-) +([0-9]+|-) +"
			//                    9               10                   11    12
			//                    Idle            Flags
			"([A-Za-z0-9]+) +([^ ]{,2})");// +default");
	QRegExp re4 = QRegExp("([TF])(.*)");
	if(re1.indexIn(line) < 0 || re3.indexIn(line) < 0)
	{
		qDebug("\n%s", line.toUtf8().data());
		qDebug("No match\n");
		return;
	}
	else
	{
				//qDebug("Match\n");
	}
			
			// 42       Neil  <None>          USA      12k  136/  86  -   -    0s    -X default  T BWN 0-9 19-19 60-60 60-3600 25-25 0-0 0-0 0-0

	aPlayer = new PlayerListing();
	aPlayer->name = re1.cap(1).trimmed();
	if(re2.indexIn(line) >= 0)
	{
		aPlayer->extInfo = re2.cap(1).trimmed();
	}
	else
		aPlayer->extInfo = "<None>";
	if(aPlayer->extInfo == "")
		aPlayer->extInfo = "<None>";

	aPlayer->country = re3.cap(1).trimmed();
	if(aPlayer->country == "--")
		aPlayer->country = "";
	aPlayer->rank = re3.cap(2).trimmed();
	fixRankString(&(aPlayer->rank));
	aPlayer->rank_score = connection->rankToScore(aPlayer->rank);
	aPlayer->wins = re3.cap(3).trimmed().toInt();
	aPlayer->losses = re3.cap(4).trimmed().toInt();
	aPlayer->observing = re3.cap(5).trimmed().toInt();
	aPlayer->playing = re3.cap(6).trimmed().toInt();
	aPlayer->idletime = re3.cap(7).trimmed();
	aPlayer->seconds_idle = idleTimeToSeconds(aPlayer->idletime);
	aPlayer->info = re3.cap(8).trimmed();

	if(re4.indexIn(line) < 0)
		aPlayer->nmatch = 0;
	else
		aPlayer->nmatch = re4.cap(1) == "T";
	aPlayer->nmatch_settings = "";
	QString nmatchString = "";
			// we want to format the nmatch settings to a readable string
	if (aPlayer->nmatch)
	{	
				// BWN 0-9 19-19 60-60 600-600 25-25 0-0 0-0 0-0
		nmatchString = re4.cap(2).trimmed();
		if ( ! nmatchString.isEmpty())
		{
					
			aPlayer->nmatch_black = (nmatchString.contains("B"));
			aPlayer->nmatch_white = (nmatchString.contains("W"));
			aPlayer->nmatch_nigiri = (nmatchString.contains("N"));					
	
					
			aPlayer->nmatch_timeMin = element(nmatchString, 2, " ", "-").toInt();
			aPlayer->nmatch_timeMax = element(nmatchString, 2, "-", " ").toInt();
			QString t1min = QString::number(aPlayer->nmatch_timeMin / 60);
			QString t1max = QString::number(aPlayer->nmatch_timeMax / 60);
			if (t1min != t1max)
				t1min.append(" to ").append(t1max) ;
					
			QString t2min = element(nmatchString, 3, " ", "-");
			QString t2max = element(nmatchString, 3, "-", " ");
			aPlayer->nmatch_BYMin = t2min.toInt();
			aPlayer->nmatch_BYMax = t2max.toInt();

			QString t3min = QString::number(t2min.toInt() / 60);
			QString t3max = QString::number(t2max.toInt() / 60);

			if (t2min != t2max)
				t2min.append(" to ").append(t2max) ;

			if (t3min != t3max)
				t3min.append(" to ").append(t3max) ;

			QString s1 = element(nmatchString, 4, " ", "-");
			QString s2 = element(nmatchString, 4, "-", " ");
			aPlayer->nmatch_stonesMin = s1.toInt();
			aPlayer->nmatch_stonesMax = s2.toInt();
					
			if (s1 != s2)
				s1.append("-").append(s2) ;
					
			if (s1 == "1")
			{
				aPlayer->nmatch_timeSystem = byoyomi;
				aPlayer->nmatch_settings = "Jap. BY : " +
						t1min + " min. + " +
						t2min + " secs./st. " ;
			}
			else
			{
				aPlayer->nmatch_timeSystem = canadian;
				aPlayer->nmatch_settings = "Can. BY : " +
						t1min + " min. + " +
						t3min + " min. /" +
						s1 + " st. ";
			}

			QString h1 = element(nmatchString, 0, " ", "-");
			QString h2 = element(nmatchString, 0, "-", " ");
			aPlayer->nmatch_handicapMin = h1.toInt();
			aPlayer->nmatch_handicapMax = h2.toInt();
			if (h1 != h2)
				h1.append("-").append(h2) ;
					
			aPlayer->nmatch_settings.append("(h " + h1 + ")") ;
		}
		else 
			aPlayer->nmatch_settings = "No match conditions";
	}

			// indicate player to be online
	aPlayer->online = true;
			
			// check if line ok, true -> cmd 'players' or 'users' preceded
	roomdispatch->recvPlayerListing(aPlayer);
	delete aPlayer;
}

		
// IGS: 49 Game 42 qGoDev is removing @ C5
void IGS_removed::handleMsg(QString line)
{
	line = line.remove(0, 2).trimmed();
	if (line.contains("is removing @"))
	{
		int game = element(line, 1, " ").toInt();
		//emit signal_removeStones(pt, game);
		BoardDispatch * boarddispatch = connection->getIfBoardDispatch(game);
		if(!boarddispatch)
		{
			qDebug("removing for game we don't have");
			return;
		}
		GameData * r = connection->getGameData(game);
		if(!r)
		{
			qDebug("Can't get game record");
			return;
		}
		
		MoveRecord * aMove = new MoveRecord();
		QString pt = element(line, 6, " ");
		aMove->flags = MoveRecord::REMOVE;
		aMove->x = (int)(pt.toAscii().at(0));
		aMove->x -= 'A';
		if(aMove->x < 9)	//no I on IGS
			aMove->x++;
		pt.remove(0,1);
		aMove->y = element(pt, 0, " ").toInt();
		/* Do we need the board size here???*/
		aMove->y = r->board_size + 1 - aMove->y;
		boarddispatch->recvMove(aMove);
		delete aMove;
	}
}
//FIXME	
//51 Say in game 432

			

// 53 Game 75 adjournment is declined
void IGS_adjourndeclined::handleMsg(QString line)
{
	line = line.remove(0, 2).trimmed();
	BoardDispatch * boarddispatch;
	if (line.contains("adjournment is declined"))
	{
		unsigned int game_nr = element(line, 1, " ").toInt();
		boarddispatch = connection->getBoardDispatch(game_nr);
		if(boarddispatch)
			boarddispatch->recvRefuseAdjourn();
		else
		{
			qDebug("Adjourn decline recv %d", game_nr);
		}
	}
}


// IGS : seek syntax, answer to 'seek config_list' command
// 63 CONFIG_LIST_START 4
// 63 CONFIG_LIST 0 60 600 25 0 0
// 63 CONFIG_LIST 1 60 480 25 0 0
// 63 CONFIG_LIST 2 60 300 25 0 0
// 63 CONFIG_LIST 3 60 900 25 0 0
// 63 CONFIG_LIST_END

// 63 OPPONENT_FOUND

// 63 ENTRY_LIST_START 5
// 63 ENTRY_LIST HKL 60 480 25 0 0 19 1 1 0
// 63 ENTRY_LIST tgor 60 480 25 0 0 19 1 1 0
// 63 ENTRY_LIST sun756 60 600 25 0 0 19 1 1 0
// 63 ENTRY_LIST horse 60 600 25 0 0 19 2 2 0
// 63 ENTRY_LIST masaeaki 60 300 25 0 0 19 1 1 0
// 63 ENTRY_LIST_END
void IGS_seek::handleMsg(QString line)
{
	line = line.remove(0, 2).trimmed();
	if (line.contains("OPPONENT_FOUND"))
	{
		QString oppname = element(line, 1, " ");
		ConsoleDispatch * console = connection->getConsoleDispatch();
		if(console)
			console->recvText("Opponent found: " + oppname);
		connection->recvSeekCancel();
	}
	else if (line.contains("CONFIG_LIST "))
	{
		SeekCondition * seekCondition = new SeekCondition();
		seekCondition->number = element(line, 1, " ").toInt();
		seekCondition->maintime = element(line, 2, " ").toInt();
		seekCondition->periodtime = element(line, 3, " ").toInt();
		seekCondition->periods = element(line, 4, " ").toInt();
		seekCondition->bline = element(line, 1, " ", "EOL");
		connection->recvSeekCondition(seekCondition);
	}
	else if (line.contains("CONFIG_LIST_START"))
	{	
		connection->recvSeekCondition(0);
	}
	else if (line.contains("ENTRY_CANCEL"))
	{	
		connection->recvSeekCancel();
	}
	else if ((line.contains("ERROR")) )
	{	
		connection->recvSeekCancel();
		ConsoleDispatch * console = connection->getConsoleDispatch();
		if(console)
			console->recvText(line.toLatin1().constData());
	}
	else if ((line.contains("ENTRY_LIST_START")) )
	{	
		//emit signal_clearSeekList();
		connection->recvSeekPlayer("", "");
	}
	else if ((line.contains("ENTRY_LIST ")) )
	{	
		QString player = element(line, 1, " ");
		QString condition = 
				element(line, 7, " ")+"x"+element(line, 7, " ")+" - " +
				QString::number(int(element(line, 2, " ").toInt()/60))+
				" +" +
				QString::number(int(element(line, 3, " ").toInt()/60)).rightJustified(3) +
				"' (" +
				element(line, 4, " ")+
				") H -"+
				element(line, 8, " ") +
				" +" +
				element(line, 9, " ");						
			
		connection->recvSeekPlayer(player, condition);
	}
}

// IGS review protocol
// 56 CREATE 107
// 56 DATA 107
// 56 OWNER 107 eb5 3k
// 56 BOARDSIZE 107 19
// 56 OPEN 107 1
// 56 KIBITZ 107 1
// 56 KOMI 107 0.50
// 56 TITLE 107 yfh22-eb5(B) IGS
// 56 SGFNAME 107 yfh22-eb5-09-03-24
// 56 WHITENAME 107 yfh22
// 56 WHITERANK 107 1d?
// 56 BLACKNAME 107 eb5
// 56 BLACKRANK 107 3k
// 56 GAMERESULT 107 B+Resign
// 56 NODE 107 1 0 0 0
// 56 NODE 107 2 1 16 4
// 56 NODE 107 3 2 16 16
// 56 NODE 107 4 1 4 4
// 56 NODE 107 5 2 4 16
// 56 NODE 107 1 0 0 0
// 56 CONTROL 107 eb5
// 56 DATAEND 107
// 56 ERROR That user's client does not support review.
// 56 INVITED_PLAY 58 yfh2test
void IGS_review::handleMsg(QString line)
{
	line = line.remove(0, 2).trimmed();
#ifdef FIXME	
	if (line.contains("CREATE"))
		aGame->number = element(line, 1, " ").toInt();
	if (line.contains("OWNER"))
		aGame->player = element(line, 2, " ");
	if (line.contains("BOARDSIZE"))
	{
		aGame->board_size = element(line, 2, " ").toInt();			
		memory = aGame->board_size;
	}
	if (line.contains("KOMI"))
		aGame->K = element(line, 2, " ");
	if (line.contains("WHITENAME"))
		aGame->wname = element(line, 2, " ");
	if (line.contains("WHITERANK"))
	{
		aGame->white_rank = element(line, 2, " ");
		fixRankString(&(aGame->white_rank));
		aGame->white_rank_score = rankToScore(aGame->white_rank);
	}
	if (line.contains("BLACKNAME"))
		aGame->black_name = element(line, 2, " ");
	if (line.contains("BLACKRANK"))
	{
		aGame->black_rank = element(line, 2, " ");
		fixRankString(&(aGame->black_rank));
		aGame->black_rank_score = rankToScore(aGame->black_rank);
	}
	if (line.contains("GAMERESULT"))
	{
		aGame->res = element(line, 2, " ");
		emit signal_gameReview(aGame);
		break;
	}
	
	if (line.contains("INVITED_PLAY"))
	{
		emit signal_reviewInvite(element(line, 1, " "), element(line, 2, " "));
		break ;
	}
	
	if (line.contains("NODE"))
	{
					
		StoneColor sc = stoneNone;
		int c = element(line, 3, " ").toInt();
					
		if (c==1)
			sc = stoneBlack;
		else if(c==2)
			sc=stoneWhite;
	
	
		emit signal_reviewNode(element(line, 1, " ").toInt(), element(line, 2, " ").toInt(), sc, element(line, 4, " ", "EOL").toInt() ,element(line, 3, " ").toInt() );
		break ;
	}
#endif //FIXME
}
