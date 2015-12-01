/**
 * Load test client
 *
 * Purpose: Connect to the chat server, send messages to "myself" 
 * periodically. Check the roundtrip echo time, show a message if
 * delay is too long.
 * 
 * Also, tell the room if there is lag, and echo any received IMs to
 * the room. Move randomly around in the room and babble intermittently
 * while doing all this.
 *
 * Input arguments:
 *   user name
 *   password
 *   community name
 *   room name (stay in the hall if none supplied)
 *   avatar file name (optional)
 *   interval between messages (seconds)
 *   number of messages to send
 *   size of message text
 *
 * Tom Lang 8/2001
 *
 */
#include <sys/stat.h>
#include <fcntl.h>
#include <VpConn.h>
#include <VpClnPlc.h>
#include <VpPrsnce.h>
#include <UbqCtrl.h>
#include <UbqOs.h>
#include <VpInfo.h>
#include <Ucm.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <glib.h>
#include <vpAttrId.h>

#define R_POS (UbqUshort)((float)(rand()) / RAND_MAX * 10000);

////////////////////
// Types & Classes //
/////////////////////
#define TITLE "VP load tester"

enum STATUS { CONNECTING, CONNECTED, DISCONNECTED };

int waitTime  = 10;
int loopCount = 10;
int msgSize  = 25;
int roomCopy   = 0;
int maxLag   = 5;
CString roomUrl;
CString homeUrl;
CString avatarFile = "avs/av1.gif";
CString fortuneFile = "zippy";
UbqBool maxLagErr = UBQ_FALSE;
int talkDelay = 5;

class Connection : public VpConnection {
 public:
  virtual void connected();  
  virtual void disconnecting(VpErrCode reason);
};

class ClientPlace : public VpClientPlace {
 public:
  ClientPlace(VpPresence* p, VpGroup* r, VpGroup* c);

  virtual void     connected();
  virtual void disconnecting(VpErrCode        reason,
			     UbqUlong         duration);
  virtual void     whispered(VpUserInfo&      whisperer,
			     const CString&   string,
			     const UbqOpaque& data,
			     const CString&   whispererFullName);
  virtual void              navigated(const VpPosition*     requestedPos,
				      VpErrCode             reason,
				      const CString&        title);
};

/////////////////
// Shared vars //
/////////////////
static Connection   theConnection;
static ClientPlace* thePlace           = 0;
static CString      communityName;
static CString      myName;
static CString      myPassword;
static STATUS       status             = DISCONNECTED;
static UbqBool      timerEventOccurred = UBQ_FALSE;
static UbqBool      hasNavigated = UBQ_FALSE;
static char*        msgText;
static char*        msgBuff;

//////////////////////
// Utility routines //
//////////////////////
static void exitFunc()
{
  ClientPlace* p = thePlace;
  thePlace = 0;
  vpTerminate();

  delete p;
}

static void memError()
{
  ubqDisplayError(TITLE, "Out of virtual memory, exiting");
}

/**
 * generate an ASCII string with a mixture of characters in it
 */
static char* makeStr(UbqUlong l)
{
  char* p = 0;
  if (l > 0) {
    p = new char[l];
    for (UbqUlong i = 0; i < l; i++)
      p[i] = 'a' + (i % 26);
    p[l-1] = 0;
  }
  return p;
}

/**
 * catch SIGALRM
 */
static void timerEvent(int signalNum, ...)
{
  ubqAssert(signalNum == SIGALRM);
  timerEventOccurred = UBQ_TRUE;
}

/**
 * set the interval timer
 */
static void setTimer( )
{
  timerEventOccurred = UBQ_FALSE;
  ubqSetSignal(SIGALRM, timerEvent);
  alarm(waitTime);
}

///////////////////////////////
// Connection implementation //
///////////////////////////////
void Connection::connected()
{
  ubqAssert(thePlace && (status == CONNECTING));
  VpFullUserName fun(myName, VP_REG_LOCAL);
  VpErrCode      rc = thePlace->signOn(theConnection, fun, myPassword);
  if (rc != VP_OK) {
    ubqDisplayError(TITLE, "cannot sign-on (%d)", (int)rc);
    status = DISCONNECTED;
  }
  const UbqList& attrList = getAttributes();
  VpAttribute* attr;
  for (UbqListIterator i = attrList.first(); attr = (VpAttribute*)i(); i++) 
  { 
    UbqUshort  type = attr->getId();
    switch(type)
    {
    case VP_ATTR_COMM_NAME:
      fprintf(stderr, "Community: %s\n", (const char *)attr->getStringValue());
      break;
    case VP_ATTR_LOBBY_URL:
      homeUrl = attr->getStringValue();
      fprintf(stderr, "Lobby URL: %s\n", (const char *)homeUrl);
      break;
    }
  }
}

void Connection::disconnecting(VpErrCode reason)
{
  if (reason)
    ubqDisplayError(TITLE, "disconnected (%d)", (int)reason);
  status = DISCONNECTED;
}

////////////////////////////////
// ClientPlace implementation //
////////////////////////////////
ClientPlace::ClientPlace(VpPresence* p, VpGroup* r, VpGroup* c)
  : VpClientPlace(p, r, c)
{
}

void ClientPlace::connected()
{
  status = CONNECTED;
  setTimer();
}

void ClientPlace::disconnecting(VpErrCode reason, UbqUlong duration)
{
  if (reason)
    ubqDisplayError(TITLE, "%s signed off (%d)", (const char *)myName, (int)reason);
  status = DISCONNECTED;
}

/**
 * completion of a navigation
 */
void ClientPlace::navigated(const VpPosition*     requestedPos,
			      VpErrCode             reason,
			      const CString&        title)
{
     if (reason == VP_ROOM_IS_FULL) {
        UbqUshort x = R_POS;	// random coordinates
        UbqUshort y = R_POS;
        roomCopy++;		// increment the room copy (replication)
        VpPosition  position(x, y);
        VpPlaceExt placeExt;
        placeExt.set(roomCopy);
        thePlace->navigate(roomUrl, "", &position, placeExt);
     }
}

/**
 * catch a whispered message (an IM)
 */
void ClientPlace::whispered(VpUserInfo&       whisperer,
			     const CString&   string,
			     const UbqOpaque& data,
			     const CString&   whispererFullName)
{
  UbqUlong memberId = whisperer.getId();
  if (memberId == getMyself()->getId()) {
    time_t timeStamp;
    sscanf(string, "%d\t", &timeStamp);
    time_t t = time(0);
    timeStamp = t - timeStamp;
    if (timeStamp > maxLag) {
      struct tm *tp = localtime(&t);
      char dt[100];
      strftime(dt, 100, "%Y%m%d %H:%M:%S", tp);
      char msg[200];
      sprintf(msg, "%s - %s - LAG %d seconds", dt, (const char *)myName, timeStamp);
      if (maxLagErr)
        fprintf(stderr, "%s\n", msg);
      else
        VpErrCode rc = thePlace->send(VP_SEND_CHAT, msg, data);
    }
  }
  else {
    CString m = whisperer.getName() + " said: " + string;
    VpErrCode rc = thePlace->send(VP_SEND_CHAT, m, data);
  }
}
int parseArgs(int argc, char* argv[])
{
  int c;
  //
  // a = avatar file
  // r = room url
  // p = password
  // u = user name
  // t = time per loop
  // l = loops
  // s = message size
  // d = max lag delay before error
  // f = fortunes file name
  // 
  // L - flag: report lag to stderr instead of room
  //
  while ((c = getopt(argc, argv, "La:d:f:l:p:r:s:t:u:")) != EOF)
    switch(c) {
       case 'a':
         avatarFile = optarg;
	 break;
       case 'd':
         maxLag = atoi(optarg);
	 break;
       case 'f':
         fortuneFile = optarg;
	 break;
       case 'l':
         loopCount = atoi(optarg);
	 break;
       case 'p':
         myPassword = optarg;
	 break;
       case 'r':
         roomUrl = optarg;
	 break;
       case 's':
         msgSize = atoi(optarg);
	 break;
       case 't':
         waitTime = atoi(optarg);
	 break;
       case 'u':
         myName = optarg;
	 break;
       case 'L':
         maxLagErr = UBQ_TRUE;
	 break;
       default:
	 fprintf(stderr, "VP Load Test usage: -u user -p password -l loops -s msg size -t time delay -a avatar file -r room URL -L to report lag to stderr [community name]\n\n");
	 break;
      }
  communityName = argv[optind];
  return 0;
}

//
// Things to say
//
GPtrArray* fortunes = NULL;
int numFortunes = 0;

void
getFortunes()
{
	CString ffile("/usr/share/games/fortune/");
	ffile += fortuneFile;
	FILE* f = fopen(ffile, "r");
	char line[500];
	GString* s = NULL;
	fortunes = g_ptr_array_new();
	while(fgets(line, 500, f)) {
		if(line[0] == '%') {
			if(s) {
				g_ptr_array_add(fortunes, s);
				s = NULL;
				numFortunes++;
			}
			continue;
		}
		if(strlen(line) == 500)	// too long
			continue;
		char* p = strchr(line, '\n');
		if(p) *p = ' ';
		if(s)
			s = g_string_append(s, line);
		else {
			s = g_string_new(line);
		}
	}
	if(s) {
		g_ptr_array_add(fortunes, s);
		s = NULL;
		numFortunes++;
	}
	fclose(f);
}

const char *
getSomethingToSay()
{
	int idx = (int)((float)(rand()) / RAND_MAX * numFortunes);
	GString* s = (GString*)(g_ptr_array_index(fortunes, idx));
	return s->str;
}

/**
 * START HERE
 */
int main(int argc, char* argv[])
{
  ubqSetExitFunc(exitFunc);
  ubqMemSetExceptionRoutine(memError);

  parseArgs( argc, argv );
  
  struct timeval tod;
  gettimeofday(&tod, NULL);
  int seed = tod.tv_sec ^ tod.tv_usec;
  srand(seed);

  getFortunes();

  //
  // message format, tab separated fields containing:
  // timestamp (secs since 1970)
  // formatted time string
  // arbitrary string, length = msgSize
  //
  msgText = makeStr(msgSize);
  msgBuff = new char[30+msgSize];
  
  ubqDisplayError(TITLE, "%s, loop interval = %d, loops = %d, msg size = %d", (const char *)myName, waitTime, loopCount, msgSize);

  VpErrCode rc = vpInitialize(0);
  if (rc != VP_OK) {
    ubqDisplayError(TITLE, "cannot initialize(%d)", (int)rc);
    ubqDoExit(1);
  }

  // Creating the (single) place
  ubqAssert(!thePlace);
  VpPresenceState* state = new VpPresenceState;
  state->setName(myName);
  state->setFullName("");
  state->setAppVersion("Virtual Places Chat Version 3.0,branding:Hal");
  thePlace = new ClientPlace(new VpPresence(state),
			     new VpGroup,
			     new VpGroup);
  ubqAssert(thePlace);

  // Start connecting; everything else is async...
  status = CONNECTING;
  if (theConnection.connect(communityName) != VP_OK) {
    ubqDisplayError(TITLE, "failed connecting to %s", (const char *)communityName);
    ubqDoExit(1);
  }

  while (status != DISCONNECTED) {
    // handle timer
    if (timerEventOccurred) {

      if (!hasNavigated) {
        hasNavigated = UBQ_TRUE;

	//
	// Navigate to the selected room URL
	//
        UbqUshort x = 500;
        UbqUshort y = 500;
        UbqUchar repCount = 1;
        VpPosition  position(x, y);
        VpPlaceExt placeExt;
        placeExt.set(repCount);
	if (roomUrl.IsEmpty()) roomUrl = homeUrl;
        thePlace->navigate(roomUrl, "", &position, placeExt);

	//
	// Put on an avatar
	//
        int a = open( avatarFile, O_RDONLY);
        if (a > -1) {
          int s = lseek( a, 0, SEEK_END );
          void *buff = malloc(s);
          if (buff) {
            lseek( a, 0, SEEK_SET );
            int b = read(a, buff, s);
            if (b == s) {
              UbqOpaque* face = new UbqOpaque(s, buff, UBQ_TRUE);
              thePlace->getMyself()->setFace(face);
            }
            close(a);
            free(buff);
          }
        }
      }

      if (loopCount-- == 0) {
        ubqDoExit(0);
      }
      ubqAssert((status == CONNECTED) && thePlace);
      sprintf( msgBuff, "%d\t%s\t%s", time(0), ubqGetCurrTimeStr(), msgText);
      VpErrCode rc = thePlace->whisper(thePlace->getMyself()->getId(),
				       msgBuff,
				       UbqOpaque());
      if (rc != VP_OK) {
	ubqDisplayError(TITLE, "Failed to whisper to myself (%d)", (int)rc);
	ubqDoExit(1);
      }

      //
      // random motion in the room
      //
      UbqUshort x = R_POS;
      UbqUshort y = R_POS;
      VpPosition  position(x, y);
      thePlace->getMyself()->move(thePlace->getRoom().getId(), position, 1);

      //
      // random babbling
      //
      if (talkDelay-- == 0) {
        talkDelay = 5;
        thePlace->send(VP_SEND_CHAT, getSomethingToSay(), UbqOpaque(0,NULL));
      }


      setTimer();
    }

    // handle i/o
    UbqFdSet r      = UcmCommMngr::getTheCm().getReadMask();
    UbqFdSet w      = UcmCommMngr::getTheCm().getWriteMask();
    int      rWidth = r.getWidth();
    int      wWidth = w.getWidth();
    int      n      = select(((rWidth > wWidth) ? rWidth : wWidth), r(), w(), 0, 0);
    ubqAssert(n);
    if (n < 0) {
      if (errno != EINTR)
	perror("select");
    }
    else if (n > 0)
      UcmCommMngr::getTheCm().analyzeMasks(r, w);
  }

  ubqDoExit(1);
}
