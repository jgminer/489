/* 
 * Copyright (c) 2014 University of Michigan, Ann Arbor.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of Michigan, Ann Arbor. The name of the University 
 * may not be used to endorse or promote products derived from this 
 * software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Author: Sugih Jamin (jamin@eecs.umich.edu)
*/
#include <stdio.h>         // fprintf(), perror(), fflush()
#include <stdlib.h>        // atoi()
#include <assert.h>        // assert()

#include <vector>           //vector that contains ptes

#include <string.h>        // memset(), memcmp(), strlen(), strcpy(), memcpy()
#include <unistd.h>        // getopt(), STDIN_FILENO, gethostname()
#include <signal.h>        // signal()
#include <netdb.h>         // gethostbyname(), gethostbyaddr()
#include <netinet/in.h>    // struct in_addr
#include <arpa/inet.h>     // htons(), inet_ntoa()
#include <sys/types.h>     // u_short
#include <sys/socket.h>    // socket API, setsockopt(), getsockname()
#include <sys/select.h>    // select(), FD_*

#include <iostream>
#include <math.h>

#include <GL/glut.h>

#include "ltga.h"
#include "netimg.h"

#include <limits.h>        // LONG_MAX
#include <deque>
#include <string>


#define net_assert(err, errmsg) { if ((err)) { perror(errmsg); assert(!(err)); } }

#define PR_PORTSEP   ':'
#define PR_UNINIT_SD  -1
#define PR_MAXFQDN   255
#define PR_QLEN      10
#define PR_LINGER    2

#define PM_VERS      0x1
#define PM_WELCOME   0x1       // Welcome peer
#define PM_RDIRECT   0x2       // Redirect peer
#define PM_SEARCH    0x4        

#define NETIS_MAXFNAME  255

#define NETIS_NUMSEG     50
#define NETIS_MSS      1440
#define NETIS_USLEEP 500000    // 500 ms

#define NETIC_WIDTH    800
#define NETIC_HEIGHT   600


using namespace std;

typedef struct {            // peer address structure
  struct in_addr peer_addr; // IPv4 address
  u_short peer_port;        // port#, always stored in network byte order
  u_short peer_rsvd;        // reserved field
} peer_t;

// Message format:              8 bit  8 bit     16 bit
typedef struct {            // +------+------+-------------+
  char pm_vers, pm_type;    // | vers | type |   #peers    |
  u_short pm_npeers;        // +------+------+-------------+
  peer_t pm_peer;          // |     peer ipv4 address     |
} pmsg_t;                   // +---------------------------+
                            // |  peer port# |   reserved  |
                            // +---------------------------+

typedef struct {            // peer table entry
  int pte_sd;               // socket peer is connected at
  char *pte_pname;          // peer's fqdn
  peer_t pte_peer;          // peer's address+port#
  bool pending;             // true if waiting for ack from peer
} pte_t;                    // ptbl entry

typedef struct{
  char pm_vers, pm_type;
  u_short peer_port;
  struct in_addr peer_addr;
  char img_nm_len;
  char img_nm[NETIS_MAXFNAME];
} query_t;

vector<pte_t> pVector;      //main peer table for this host
vector<pte_t> tryVector;    //vector of received pte's to try
vector<pte_t> peerDecline;  //peers recvd redirect
pte_t pteQuery;             //pte_t listening for query response
deque<long> circBuff;        //buffer of calculated IDs for each new query
query_t currCheck;          //current received query to check
query_t sendQuery;          //send this query out when possible - set in args
int MAXPEERS = 6;           //default = 6
int MAXSEND = 6;            //always 6
int BUFSIZE = 20;
int maxsd = 0;                  //global for faster calculation

LTGA image;
imsg_t imsg;
long img_size;
long img_offset;
char *image_char;

char fname[NETIS_MAXFNAME] = { 0 };
char searchName[NETIS_MAXFNAME] = { 0 };

char NETIS_IMVERS = 1;    //always 1

int wd;                   /* GLUT window handle */
GLdouble width, height;   /* window width and height */

int recv_image_td = 0;
int recv_imsg_sd = 0;




void
peer_usage(char *progname)
{
  fprintf(stderr, "Usage: %s [ -p peerFQDN.port ]\n", progname); 
  exit(1);
}

bool in_Table(pte_t *pte, bool useDecline, int *location, bool ifAccept){
  int LARGEST;
  if (useDecline) LARGEST = (int) peerDecline.size();
  else LARGEST = (int) pVector.size();
  if (ifAccept) LARGEST-=1; //don't check last element if added by accept
  for(int i = 0; i < LARGEST; i++){
    if (!useDecline &&
    (pte->pte_peer.peer_addr.s_addr == pVector[i].pte_peer.peer_addr.s_addr &&
    pte->pte_peer.peer_port == pVector[i].pte_peer.peer_port)){
      *location = i;
      return true;
    }
    if (useDecline &&
    (pte->pte_peer.peer_addr.s_addr == peerDecline[i].pte_peer.peer_addr.s_addr &&
    pte->pte_peer.peer_port == peerDecline[i].pte_peer.peer_port)){
      *location = i;
      return true;
    }
  }
  return false;
}

int
peer_args(int argc, char *argv[], char *pname, u_short *port)
{
  char c, *p;
  extern char *optarg;

  // net_assert(!pname, "peer_args: pname not allocated");  shouldn't matter
  // net_assert(!port, "peer_args: port not allocated");

  while ((c = getopt(argc, argv, "p:n:f:q:")) != EOF) {
    switch (c) {
    case 'p':
      for (p = optarg+strlen(optarg)-1;     // point to last character of addr:port arg
           p != optarg && *p != PR_PORTSEP; // search for ':' separating addr from port
           p--);
      net_assert((p == optarg), "peer_args: peer addressed malformed");
      *p++ = '\0';
      *port = htons((u_short) atoi(p)); // always stored in network byte order

      net_assert((p-optarg > PR_MAXFQDN), "peer_args: FQDN too long");
      strcpy(pname, optarg);
      break;
    case 'n':
      MAXPEERS = atoi(optarg); //change from 6 if no value given
      break;
    case 'f':
      net_assert((strlen(optarg) >= NETIS_MAXFNAME), "netis_args: image file name too long");
      strcpy(fname, optarg);
      break;
    case 'q':
      net_assert((strlen(optarg) >= NETIS_MAXFNAME), "netis_args: image file name too long");
      strcpy(searchName, optarg);
      break;
    default:
      return(1);
      break;
    }
  }

  return (0);
}
/*
 * Terminates process on error.
 * Returns the bound socket id.
*/
int
peer_setup(u_short port)
{
  /* create a TCP socket, store the socket descriptor in "sd" */
  //compiled using  install g++-multilib
  int sd;
  if ((sd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)) < 0) {
    perror("socket");
    printf("Failed to create socket\n");
    abort();
  }

  if (sd >= maxsd) maxsd = sd; //if this is the largest sd, change it!
  /* initialize socket address */
  struct sockaddr_in self;
  memset((char *) &self, 0, sizeof(struct sockaddr_in));
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = INADDR_ANY;
  self.sin_port = port; // in network byte order

  /* reuse local address so that bind doesn't complain
     of address already in use. */
  int yes = 1;
  int test = setsockopt(sd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

  if (test < 0){
    perror("setting reuse failed");
    abort();
  }

  /* bind address to socket */
  if (bind(sd, (struct sockaddr*) &self, sizeof(self)) < 0){
    // cout << "always" << endl;
    perror("bind");
    abort();
  }

  /* listen on socket */
  if (listen(sd, PR_QLEN) < 0) {
    perror("error listening");
    abort();
  }

  /* return socket id. */
  return (sd);
}

int
peer_accept(int sd, pte_t *pte)
{
  struct sockaddr_in peer;

  int td;
  socklen_t sockaddr_in_size = sizeof(peer);
  td = accept(sd, (struct sockaddr *) &peer, &sockaddr_in_size) ;
  if (td < 0) {
      perror("error accepting connection");
      abort();
  }

  pte->pte_sd = td;

  if (td >= maxsd) maxsd = td; //if this is the largest sd, change it!

  /* make the socket wait for PR_LINGER time unit to make sure
     that all data sent has been delivered when closing the socket */
  linger lingtmp;
  socklen_t linlen = sizeof(lingtmp);
  lingtmp.l_onoff = 1;
  lingtmp.l_linger = PR_LINGER;
  if (setsockopt(td, SOL_SOCKET, SO_LINGER, &lingtmp, linlen) < 0){
    perror("setting socket linger time failed");
    abort();
  }


  /* store peer's address+port# in pte */
  memcpy((char *) &pte->pte_peer.peer_addr, (char *) &peer.sin_addr, 
         sizeof(struct in_addr));
  pte->pte_peer.peer_port = peer.sin_port; /* stored in network byte order */

  return (pte->pte_sd);
}

/*
 * If there's any error in sending, closes the socket td.
 * In all cases, returns the error message returned by send().
*/
int peer_ack(int td, char type, pte_t *sendTo)
{
  int err;
  int copiedBytes = 0;

  int VectorSize = pVector.size();

  unsigned char byte_arr[sizeof(pmsg_t) +(VectorSize-1)*sizeof(peer_t)];
  memset(byte_arr, 0, sizeof(pmsg_t)); //make sure at least the initial part is empty

  pmsg_t *sendThis = new pmsg_t;
  sendThis->pm_vers = PM_VERS;
  sendThis->pm_type = type;
  //if peer is null, set num peers to 0
  //sendThis->pm_npeers = (u_short) VectorSize < (u_short) MAXPEERS ? VectorSize : MAXPEERS;
  sendThis->pm_peer.peer_addr.s_addr = 0;
  sendThis->pm_peer.peer_rsvd = 0;
  sendThis->pm_peer.peer_port = 0;

  int actual_sent = 0;
  //only send up to 6!!!!
  for (uint i = 0; i < pVector.size(); i++){
    if (actual_sent >= 6) break;
    if (copiedBytes < (int)sizeof(pmsg_t)){
      if(strcmp(pVector[i].pte_pname, sendTo->pte_pname)   //if we are already peered, don't returnb
      || (pVector[i].pte_peer.peer_port != sendTo->pte_peer.peer_port)){
        sendThis->pm_peer = pVector[i].pte_peer;
        memcpy(byte_arr, sendThis, sizeof(pmsg_t));
        copiedBytes += sizeof(pmsg_t);
        actual_sent++;
      }
    }
    else {

      if(strcmp(pVector[i].pte_pname, sendTo->pte_pname)   //if we are already peered, don't returnb
      || (pVector[i].pte_peer.peer_port != sendTo->pte_peer.peer_port)){
        memcpy(&byte_arr[copiedBytes], &pVector[i].pte_peer, sizeof(peer_t));
        copiedBytes += sizeof(peer_t);
        actual_sent++;
      }
    }
  }
  int partial = 4; //parial is only 32 bits
  sendThis->pm_npeers = actual_sent;
  if (actual_sent == 0) copiedBytes = partial; //keeps it below
  memcpy(&byte_arr[0], sendThis, sizeof(pmsg_t));
  //assert((uint)copiedBytes == sizeof(byte_arr)); bad assert - dynamically sized
  err = send(td, &byte_arr, copiedBytes, 0);

  if (err < 0){
    perror("error acking to peer");
    abort();
  }
  delete sendThis;
  return(err);
}

/*
 * On success, returns 0.
 * On error, terminates process.
 */
 //doubles as peer_setup w/o listening for query port.
int peer_connect(pte_t *pte, sockaddr_in *self, bool connect_){
  int sd = 0;
  if ((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
    perror("opening TCP socket");
    abort();
  }

  pte->pte_sd = sd;

  if (sd >= maxsd) maxsd = sd; //if this is the largest sd, change it!

  /* reuse local address so that the call to bind in peer_setup(), to
     bind to the same ephemeral address, doesn't complain of address
     already in use. */
  int yes = 1;
  int test = setsockopt(sd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(int));
  if (test < 0){
    perror("setting reuse failed");
    return -1;
  }
  struct sockaddr_in bin;
  memset(&bin, 0, sizeof(sockaddr_in));
  bin.sin_family = AF_INET;
  bin.sin_addr.s_addr = INADDR_ANY;   //NOT SURE IF CORRECT - maybe should be dest address?
  bin.sin_port = self->sin_port;     //use the port that we are listening on for bind

  /* bind my LISTENING address:port to socket */
  if (bind(sd, (struct sockaddr*) &bin, sizeof(bin)) < 0){
    perror("bind");
    abort();
  }

    socklen_t selflen = sizeof(struct sockaddr_in);
  if (getsockname(sd, (struct sockaddr*) self, &selflen) < 0){
    perror("getsockname");
    abort();
  }
  if (!connect_){ //if not connecting as in the query bind case, return   
    return(0);
  }
  /* initialize socket address with destination peer's IPv4 address and port number . */
  struct sockaddr_in cin;
  //MAY NEED THESE:
  struct hostent *host = gethostbyname(pte->pte_pname);
  unsigned int server_addr = *(unsigned long *) host->h_addr_list[0];
  //SET THE IP ADDRESS!!!!
  pte->pte_peer.peer_addr.s_addr = *(unsigned long *) host->h_addr_list[0];

  memset(&cin, 0, sizeof(sockaddr_in));
  cin.sin_family = AF_INET;
  cin.sin_addr.s_addr = server_addr;
  cin.sin_port = pte->pte_peer.peer_port; 

  /* connect to destination peer. */
  if (connect(sd, (struct sockaddr *) &cin, sizeof(cin)) != 0){
      perror("failed to connect to server");
      //peerDecline.push_back(*pte);
      abort();
  }

  pte->pending = true;  //change pending
  pVector.push_back(*pte);    //push onto the peer table!

  return(0);
}

void print_peer(pte_t *p){
  struct hostent *phost;
    //determines the host name, print out port as well
  phost = gethostbyaddr((char *) &p->pte_peer.peer_addr,
                        sizeof(struct in_addr), AF_INET);

  char *print = (phost && phost->h_name) ? phost->h_name :
           inet_ntoa(p->pte_peer.peer_addr);
  cout << "  which is peered with: " << print << ":" <<
  ntohs(p->pte_peer.peer_port) << "\n";
}

int peer_recv(pte_t *target, uint npeers)
{
  pmsg_t msg;
  memset(&msg, 0, sizeof(pmsg_t));
  size_t partial = 4;  //minimum to recv is 32 bits - pmsg_t w/o peer_t
  int bytes_recv = recv(target->pte_sd, &msg, partial, 0);
  if (bytes_recv <= 0){
    close(target->pte_sd);
    return(bytes_recv);
  }
  while((uint)bytes_recv < partial){
    bytes_recv += recv(target->pte_sd, &(msg)+bytes_recv, partial-bytes_recv, 0);
  }

  if (msg.pm_type == PM_SEARCH){
    //copy what we have received so far into the new data structure
    memset(&currCheck, 0, sizeof(query_t));
    memcpy(&currCheck, &msg, bytes_recv);
    bytes_recv = partial;

    unsigned char test[1000];
    memset(test, 0, 1000);
    size_t min = 5; //next minimum is 40bits
    // bytes_recv = recv(target->pte_sd, &currCheck[0]+bytes_recv, min, 0); //recv into tmp
    while((uint)bytes_recv < min+partial){
      //bytes_recv += recv(target->pte_sd, (char *)(&(currCheck))+bytes_recv, sizeof(query_t)-bytes_recv, 0); //recv into tmp
      bytes_recv += recv(target->pte_sd, (char *)(&(currCheck))+bytes_recv, min+partial-bytes_recv, 0); //recv into tmp
    }
    //should be able to get the length now
    int name_len = currCheck.img_nm_len;//atoi(&(currCheck.img_nm_len));
    while(bytes_recv < name_len+min+partial){
      bytes_recv += recv(target->pte_sd, (char *)(&(currCheck))+bytes_recv, name_len+min+partial-bytes_recv, 0); //recv into tmp
    }
    int sum = 0;
    long total = 0;
    int bytesLeft = sizeof(query_t);
    // for(int i = 0; i < sizeof(query_t)/sizeof(int); i++){      //copy 4 bytes at a time, 
    for(int i = 0; i < ceil(sizeof(query_t)/sizeof(int)); i++){ 
      int copyAmt = bytesLeft < sizeof(int) ? bytesLeft : sizeof(int);
      memcpy(&sum, (char*)(&(currCheck))+(i*sizeof(int)), copyAmt);
      bytesLeft -= copyAmt;
      total += sum;
      sum = 0;
    }
    bool found = false;
    for (int i = 0; i < circBuff.size(); i++){
      if (circBuff[i] == total){
        found = true;
        break;
      }
    }

    if (found){
      cout << "Duplicate search query, dropping" << endl;
      return (3);
    }
    circBuff.push_back(total);
    if (circBuff.size() > BUFSIZE) circBuff.pop_front(); //makes buffer circular by popping when at cap.

    cout << "Received unique search query" << endl; //still have to write this part memcpy 32 bits in loop
    
    return(2); //specific return value
  }

  //fprintf(stderr, "Received ack from %s:%d\n", target->pte_pname, ntohs(target->pte_peer.peer_port));
  cout << "Received ack from " << target->pte_pname << ":" 
  << ntohs(target->pte_peer.peer_port) << endl;
  if (msg.pm_vers != PM_VERS) {
      //fprintf(stderr, "unknown message version.\n");
      cout << "unknown message version.\n";
      
      return(-1);
  }

  if (msg.pm_type == PM_RDIRECT) {
    // inform user if message is a redirection
    //fprintf(stderr, "Join redirected, try to connect to the peer above.\n");
    cout << "Join redirected, try to connect to the peer above.\n";
    // add to DECLINED peers
    peerDecline.push_back(*target);
    // remove from PEER TABLE pVector
    pVector.erase(pVector.begin()+npeers);
  }

  //attempt to parse out the pm_npeers
  u_short peers = msg.pm_npeers;

  for (int i = 0; i < peers; i++){
    //reset bytes recvd
    pte_t tmp;
    bytes_recv = 0;
    memset(&tmp, 0, sizeof(pte_t)); //zero-out temp
    bytes_recv = recv(target->pte_sd, &tmp.pte_peer, sizeof(peer_t), 0); //recv into tmp
    while((uint)bytes_recv < sizeof(peer_t)){
      bytes_recv += recv(target->pte_sd, &(tmp.pte_peer)+bytes_recv, sizeof(peer_t)-bytes_recv, 0); //recv into tmp
    }
    tryVector.push_back(tmp); //push onto back of try vector
    print_peer(&tmp); //prints out the peer
  }

  return (1);
}



//returns true on receipt of WELCOME or RDIRECT
int recv_handler(pte_t *target, uint npeers){
  //int i;get length of byte array
  int err;

  err = peer_recv(target, npeers); // Task 2: fill in the functions peer_recv() above
  net_assert((err < 0), "peer: peer_recv");
  if (err == 0) {
    // if connection closed/error by peer, remove peer table entry
    return false;
  }

  else if (err == 1){
    target->pending = false; ///change pending to false now that we have received an ack from the other peer.
    return 1;
  }

  else if (err == 2) {
    //was a query packet
    return 2;
  }
  else return 3;
}

bool send_RDIRECT(int sd, pte_t *redirected, bool acceptedPrior){
  int err;
  struct hostent *phost;

  if (!acceptedPrior){
    peer_accept(sd, redirected);
  }

  err = peer_ack(redirected->pte_sd, PM_RDIRECT, redirected);

  err = (err != sizeof(pmsg_t));
  net_assert(err, "peer: peer_ack redirect");

  /* log connection */
  /* get the host entry info on the connected host. */
  phost = gethostbyaddr((char *) &redirected->pte_peer.peer_addr,
                      sizeof(struct in_addr), AF_INET);

  char *print = (phost && phost->h_name) ? phost->h_name:
          inet_ntoa(redirected->pte_peer.peer_addr);
  cout << "Peer table full: " << print << ":" << 
          ntohs(redirected->pte_peer.peer_port) << "redirected\n";

  /* closes connection */
  close(redirected->pte_sd);
  return true;
}

bool accept_handler(int sd, uint npeers){
  //int err;
  struct hostent *phost;

  peer_accept(sd, &pVector[npeers]);

  /* log connection */
  /* get the host entry info on the connected host. */
  phost = gethostbyaddr((char *) &pVector[npeers].pte_peer.peer_addr,
                      sizeof(struct in_addr), AF_INET);
  strcpy(pVector[npeers].pte_pname,
         ((phost && phost->h_name) ? phost->h_name:
          inet_ntoa(pVector[npeers].pte_peer.peer_addr)));

  int location;
  if (in_Table(&pVector[npeers], false, &location, true)) { //if already in peer table
    if (pVector[location].pending){
      int in_place = pVector[location].pte_peer.peer_addr.s_addr+pVector[location].pte_peer.peer_port;
      int attempting = pVector[npeers].pte_peer.peer_addr.s_addr+pVector[npeers].pte_peer.peer_port;
      if (in_place < attempting){
        send_RDIRECT(sd, &pVector[npeers], true);
      }
      else peer_ack(pVector[npeers].pte_sd, PM_WELCOME, &pVector[npeers]);
    } //tie breaker

    else { //this is an error
      send_RDIRECT(sd, &pVector[location], true);
    }
    return false;
  }

  /*err =*/ peer_ack(pVector[npeers].pte_sd, PM_WELCOME, &pVector[npeers]);

  cout << "Connected from peer " << pVector[npeers].pte_pname << ":" 
  << ntohs(pVector[npeers].pte_peer.peer_port)<< "\n";

  pVector[npeers].pending = false; //ensure that pending is still false.

  return true;
}

bool connect_handler(pte_t *connect_pte, sockaddr_in *self){
  int dummy;
  struct hostent *phost;
  if (connect_pte->pte_peer.peer_addr.s_addr){
    phost = gethostbyaddr((char *) &connect_pte->pte_peer.peer_addr,
                      sizeof(struct in_addr), AF_INET);
  }
  else {
    phost = gethostbyname(connect_pte->pte_pname);
  }

  if (phost && phost->h_name) strcpy(connect_pte->pte_pname, phost->h_name);
  else strcpy(connect_pte->pte_pname, inet_ntoa(connect_pte->pte_peer.peer_addr));

  if (in_Table(connect_pte, false, &dummy, false)) { //if already in peer table
    return false;
  }
  dummy = -1;
  if (in_Table(connect_pte, true, &dummy, false)) { //if already in declined table
    return false;
  }

  sockaddr_in addr_tmp;
  sockaddr_in *sk_addr_tmp = &addr_tmp; //don't overwrite self
  /* connect to peer in pte[0] */
  peer_connect(connect_pte, self, true);  // Task 2: fill in the peer_connect() function above
  socklen_t selflen = sizeof(*sk_addr_tmp);
  getsockname(connect_pte->pte_sd, (struct sockaddr*) sk_addr_tmp, &selflen);


  cout << "Connected to peer " << connect_pte->pte_pname << ":" 
  << ntohs(connect_pte->pte_peer.peer_port)<< "\n";

  return true;
}
bool ack_query(int td, query_t *ackThis){
  //num bytes to send is 9 + length of the string (including null char)
  int name_len = ackThis->img_nm_len;
  int send_bytes = 9 + name_len;
  int err = send(td, ackThis, send_bytes, 0);
  // int err2 = send(td, &ackThis->pm_type, sizeof(char), 0);
  // int err3 = send(td, &ackThis->peer_port, sizeof(u_short), 0);
  // int err4 = send(td, &ackThis->peer_addr, sizeof(struct in_addr), 0);
  if (err < 0 ){// || err2 < 0 || err3 < 0 || err < 0){ 
    cout << "Issue sending query \n";
    return false;
  }
  return true;
}
///////%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%///////////
////////%%%%%%%%%%%%%%IMAGE STUFF%%%%%%%%%%%%%%%%%////////////
void
netis_imginit(char *fname, LTGA *image, imsg_t *imsg, long *img_size)
{
  int alpha, greyscale;
  double img_dsize;
  
  image->LoadFromFile(fname);
  net_assert((!image->IsLoaded()), "netis_imginit: image not loaded");

  cout << "Image: " << endl;
  cout << "     Type   = " << LImageTypeString[image->GetImageType()] 
       << " (" << image->GetImageType() << ")" << endl;
  cout << "     Width  = " << image->GetImageWidth() << endl;
  cout << "     Height = " << image->GetImageHeight() << endl;
  cout << "Pixel depth = " << image->GetPixelDepth() << endl;
  cout << "Alpha depth = " << image->GetAlphaDepth() << endl;
  cout << "RL encoding  = " << (((int) image->GetImageType()) > 8) << endl;
  /* use image->GetPixels()  to obtain the pixel array */

  img_dsize = (double) (image->GetImageWidth()*image->GetImageHeight()*(image->GetPixelDepth()/8));
  net_assert((img_dsize > (double) LONG_MAX), "netis: image too big");
  *img_size = (long) img_dsize;

  imsg->im_vers = NETIS_IMVERS;
  imsg->im_depth = (unsigned char)(image->GetPixelDepth()/8);
  imsg->im_width = htons(image->GetImageWidth());
  imsg->im_height = htons(image->GetImageHeight());
  alpha = image->GetAlphaDepth();
  greyscale = image->GetImageType();
  greyscale = (greyscale == 3 || greyscale == 11);
  if (greyscale) {
    imsg->im_format = htons(alpha ? GL_LUMINANCE_ALPHA : GL_LUMINANCE);
  } else {
    imsg->im_format = htons(alpha ? GL_RGBA : GL_RGB);
  }

  return;
}
  
void
netis_imgsend(int td, imsg_t *imsg, LTGA *image, long img_size)
{
  int segsize;
  char *ip;
  int bytes;
  long left;

  /* Task 2: YOUR CODE HERE
   * Send the imsg packet to client connected to socket td.
   */

  int sent_bytes = send(td, imsg, sizeof(imsg), 0);
  
  //cout << sent_bytes;
  if (sent_bytes < 0){
    perror("send imsg failed");
    abort();
   }

  segsize = img_size/NETIS_NUMSEG;                     /* compute segment size */
  segsize = segsize < NETIS_MSS ? NETIS_MSS : segsize; /* but don't let segment be too small*/

  ip = (char *) image->GetPixels();    /* ip points to the start of byte buffer holding image */

  for (left = img_size; left; left -= bytes) {  // "bytes" contains how many bytes was sent
                                                // at the last iteration.

    /* Task 2: YOUR CODE HERE
     * Send one segment of data of size segsize at each iteration.
     * The last segment may be smaller than segsize
    */
    if (left < segsize){
      segsize = left;
    }
    //cout << bytes << endl;
    bytes = send(td, &ip[img_size-left], segsize, 0);

    if (bytes < 0){
      perror("send image data failed");
      abort();
    }

    fprintf(stderr, "netis_send: size %d, sent %d\n", (int) left, bytes);
    usleep(NETIS_USLEEP);
  }

  return;
}

void
netic_recvimsg()
{
  int bytes_recvd;
  char imsg_buf[sizeof(imsg)];
  bytes_recvd = recv(recv_image_td, imsg_buf, sizeof(imsg), 0);
 
  if(imsg_buf[0] != IM_VERS){
    perror("version incorrect");
    abort();
  }

  while (bytes_recvd > 0){
    bytes_recvd = recv(recv_image_td, imsg_buf, sizeof(imsg)-bytes_recvd, 0);
  }

  //fix endianess of imsg params
  char tmp[1];
  for (int i=2; i < 7; i+=2){
      memcpy(&tmp, &imsg_buf[i], 2);
      unsigned short tmp1 = ntohs(*(unsigned short*) tmp);
      memcpy(&imsg_buf[i], &tmp1, 2);
  }
  
  memcpy(&imsg, imsg_buf, sizeof(imsg));

  return;
}

void
netic_imginit()
{
  int tod;
  double img_dsize;

  img_dsize = (double) (imsg.im_height*imsg.im_width*(u_short)imsg.im_depth);
  net_assert((img_dsize > (double) LONG_MAX), "netic: image too big");
  img_size = (long) img_dsize;                 // global
  image_char = (char *)malloc(img_size*sizeof(char));

  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glGenTextures(1, (GLuint*) &tod);
  glBindTexture(GL_TEXTURE_2D, tod);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); 
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); 
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE); 
  glEnable(GL_TEXTURE_2D);
}
void
netic_recvimage(void)
{
   
  // img_offset is a global variable that keeps track of how many bytes
  // have been received and stored in the buffer.  Initialy it is 0.
  //
  // img_size is another global variable that stores the size of the image.
  // If all goes well, we should receive img_size bytes of data from the server.
  if (img_offset <  img_size) { 
    /* Task 1: YOUR CODE HERE
     * Receive as much of the remaining image as available from the network
     * put the data in the buffer pointed to by the global variable 
     * "image" starting at "img_offset".
     *
     * For example, the first time this function is called, img_offset is 0
     * so the received data is stored at the start (offset 0) of the "image" 
     * buffer.  The global variable "image" should not be modified.
     *
     * Update img_offset by the amount of data received, in preparation for the
     * next iteration, the next time this function is called.
     */

    int recvd = recv(recv_image_td, &(image_char[img_offset]), img_size-img_offset, 0);
    img_offset += recvd;
    
    /* give the updated image to OpenGL for texturing */
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint) imsg.im_format,
                 (GLsizei) imsg.im_width, (GLsizei) imsg.im_height, 0,
                 (GLenum) imsg.im_format, GL_UNSIGNED_BYTE, image_char);
    /* redisplay */
    glutPostRedisplay();
  }

  return;
}


void 
netic_display(void)
{
  glPolygonMode(GL_FRONT, GL_POINT);
  
  /* If your image is displayed upside down, you'd need to play with the
     texture coordinate to flip the image around. */
  glBegin(GL_QUADS); 
  glTexCoord2f(0.0,1.0); glVertex3f(0.0, 0.0, 0.0);
  glTexCoord2f(0.0,0.0); glVertex3f(0.0, height, 0.0);
  glTexCoord2f(1.0,0.0); glVertex3f(width, height, 0.0);
  glTexCoord2f(1.0,1.0); glVertex3f(width, 0.0, 0.0);
  glEnd();

  glFlush();
}

void
netic_reshape(int w, int h)
{
  /* save new screen dimensions */
  width = (GLdouble) w;
  height = (GLdouble) h;

  /* tell OpenGL to use the whole window for drawing */
  glViewport(0, 0, (GLsizei) w, (GLsizei) h);

  /* do an orthographic parallel projection with the coordinate
     system set to first quadrant, limited by screen/window size */
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluOrtho2D(0.0, width, 0.0, height);
}


void
netic_kbd(unsigned char key, int x, int y)
{
  switch((char)key) {
  case 'q':
  case 27:
    glutDestroyWindow(wd);
#ifdef _WIN32
    WSACleanup();
#endif
    exit(0);
    break;
  default:
    break;
  }

  return;
}

void
netic_glutinit(int *argc, char *argv[])
{

  width  = NETIC_WIDTH;    /* initial window width and height, */
  height = NETIC_HEIGHT;         /* within which we draw. */

  glutInit(argc, argv);
  glutInitDisplayMode(GLUT_SINGLE | GLUT_RGBA);
  glutInitWindowSize((int) NETIC_WIDTH, (int) NETIC_HEIGHT);
  wd = glutCreateWindow("Netimg Display" /* title */ );   // wd global
  glutDisplayFunc(netic_display);
  glutReshapeFunc(netic_reshape);
  glutKeyboardFunc(netic_kbd);
  glutIdleFunc(netic_recvimage); // Task 1

  return;
} 

///////%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%///////////
///////%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%///////////


int main(int argc, char *argv[])
{
  fd_set rset;
  int i, sd;
  struct sockaddr_in self;                            // the address of this host


  //store the host's FQDN:
  char tmpFQDN[PR_MAXFQDN+1];
  memset(tmpFQDN, 0, PR_MAXFQDN+1); //zeros out
  // char *tmpFQDN = FQDN;

  tryVector.resize(1);
  char tmp[PR_MAXFQDN+1]; //includes space for null
  memset(&tmp, 0, PR_MAXFQDN+1); //zeros out
  tryVector[0].pte_pname = tmp;

  tryVector[0].pte_peer.peer_port = 0;
  // parse args, see the comments for peer_args()
  if (argc > 1){
    if (peer_args(argc, argv, tryVector[0].pte_pname, &tryVector[0].pte_peer.peer_port)) {
      peer_usage(argv[0]);
    }
  }

  // if the arguments weren't actually used for peering right away
  if (tryVector[0].pte_peer.peer_port == 0) tryVector.clear();
  else assert(argc == 1 || tryVector[0].pte_peer.peer_port );
  // // init (the rest!) of the data
  memset((char *) &self, 0, sizeof(struct sockaddr_in));

  /* setup and listen on connection */
  sd = peer_setup(self.sin_port);  // Task 1: fill in the peer_setup() function above
  if (!self.sin_port) {

    socklen_t selflen = sizeof(self);
    if (getsockname(sd, (struct sockaddr*) &self, &selflen) < 0){
      perror("getsockname");
      abort();
    }
  }

  int err_get = gethostname(tmpFQDN, PR_MAXFQDN+1);
  if (err_get < 0){
    perror("trouble getting host name");
    abort();
  }

  struct hostent *getIP = gethostbyname(tmpFQDN);
  if (getIP && getIP->h_addr_list[0]){
    self.sin_addr.s_addr = *(unsigned long*) getIP->h_addr_list[0];
  }
  //cout << "my IP: " << inet_ntoa(self.sin_addr);
  cout << "This peer address is " << tmpFQDN << ":" <<
  ntohs(self.sin_port) << "\n";

  /* connect to peer if in args*/
  if (argc > 1){
    if (tryVector.size() > 0) {
      connect_handler(&tryVector[0], &self);
    }
  }
  //clear tryVector again since we don't want the arg in it
  tryVector.clear();

  //init image if we hold one - if fname is set to something besides 0
  if (strcmp(fname, "")){
    netis_imginit(fname, &image, &imsg, &img_size);
  }

  //init socket to listen on if we are querying:
  char tmp2[PR_MAXFQDN+1]; //includes space for null
  memset(&tmp2, 0, PR_MAXFQDN+1); //zeros out
  pteQuery.pte_pname = tmp2;
  if (strcmp(searchName, "")){
    struct sockaddr_in selfForPic;                            // the address of this host
    memset((char *) &selfForPic, 0, sizeof(struct sockaddr_in));
    int xd = peer_setup(0);
    pteQuery.pte_sd = xd;

    socklen_t selflenPic = sizeof(selfForPic);
    if (getsockname(xd, (struct sockaddr*) &selfForPic, &selflenPic) < 0){
      perror("getsockname");
      abort();
    }
    char tmp3[PR_MAXFQDN+1]; //includes space for null
    memset(&tmp3, 0, PR_MAXFQDN+1); //zeros out
    string getLen = searchName;
    sendQuery.pm_vers = PM_VERS;
    sendQuery.pm_type = PM_SEARCH;
    sendQuery.peer_port = selfForPic.sin_port;
    sendQuery.peer_addr = self.sin_addr;
    sendQuery.img_nm_len = getLen.length()+1;

    // char setChar[NETIS_MAXFNAME+1] = {0};
    // sendQuery.img_nm = setChar;
    memset(sendQuery.img_nm, 0, NETIS_MAXFNAME);
    strcpy(sendQuery.img_nm, searchName);
  }

  while(1) {
    /* set all the descriptors to select() on */
    FD_ZERO(&rset);

    FD_SET(sd, &rset);           // or the listening socket,
    FD_SET(pteQuery.pte_sd, &rset); //or the query listening socket

    for (i = 0; i < (int)pVector.size(); i++) {
      if (pVector[i].pte_sd > 0) {
        FD_SET(pVector[i].pte_sd, &rset);  // or the peer connected sockets
      }
    }

    struct timeval timeout;
    timeout.tv_usec = 100000; timeout.tv_sec = 0;
    select(maxsd+1, &rset, NULL, NULL, &timeout);

    // if peer wants to join with me on sd - permanent listen socket
    if (FD_ISSET(sd, &rset)) {
      if (pVector.size() < (uint) MAXPEERS) {
        pVector.resize(pVector.size()+1); //resize to one larger
        //init pending to false
        pVector[pVector.size()-1].pending = false;
        char * tmp = (char *) malloc(PR_MAXFQDN);
        pVector[pVector.size()-1].pte_pname = tmp;
        accept_handler(sd, pVector.size()-1); //access at last element
      }

      else {
        //send redirect
        pte_t dummy_redirect; //since send is expecting a place for one
        char tmp[PR_MAXFQDN+1];
        memset(&tmp, 0, PR_MAXFQDN+1);
        dummy_redirect.pte_pname = tmp;
        send_RDIRECT(sd, &dummy_redirect, false);
      }
    }

    // receiving RDIRECT or WELCOME
    for (uint p = 0; p < pVector.size(); p++) {
      if (pVector[p].pte_sd > 0 && FD_ISSET(pVector[p].pte_sd, &rset)) {
        // a message arrived from a connected peer, receive it
        int recv_type = recv_handler(&pVector[p], p); // don't know how to use result
        if (!recv_type){
          //remove from pVector if the connection is closed
          pVector.erase(pVector.begin()+p);
        }
        if (recv_type == 2){ //recvd query, check if i have image. 
                             //if not, send out to all except recvd on
          cout << "received!" << endl;
          if (!strcmp(currCheck.img_nm, fname)){
            cout << "found image here!\n";
            pte_t dum;
            dum.pte_peer.peer_port = currCheck.peer_port;
            dum.pte_peer.peer_addr = currCheck.peer_addr;
            char allo[PR_MAXFQDN+1]; //includes space for null
            memset(&allo, 0, PR_MAXFQDN+1); //zeros out
            dum.pte_pname = allo;
            connect_handler(&dum, &self);
            netis_imgsend(dum.pte_sd, &imsg, &image, img_size); // Task 2
            close(dum.pte_sd);

          }
          for (int i = 0; i < (int)pVector.size(); i++){
            cout << "about to forward query \n";
            if (p == (uint)i) continue; //if the same as one recvd on, skip
            ack_query(pVector[i].pte_sd, &currCheck);
          }
        }
        if (recv_type == 3) break; // duplicate search packet
        
        // must try to fill up table with peers
        for (int i = 0; i < (int)tryVector.size(); i++){
          //if the table is full
          if ((int)pVector.size() == MAXPEERS){
            break;
          }
          //otherwise, try to connect to it
          char allo[PR_MAXFQDN+1]; //includes space for null
          memset(&allo, 0, PR_MAXFQDN+1); //zeros out
          tryVector[i].pte_pname = allo;
          connect_handler(&tryVector[i], &self);
        }
        //erase tryVector for next time
        tryVector.clear(); //TODO - right place to clear?
      }
    }

    //if it is in the args
    //if looking for an image, send out the query packet to peers already in pVector
    if (strcmp(searchName, "")){
      if (pVector.size() > 0){   ///FIX THIS- temporary fix to wait until one peer is there!!!!
        for (int i = 0; i < (int)pVector.size(); i++){
          cout << "about to try sending query \n";
          ack_query(pVector[i].pte_sd, &sendQuery);
        }
        //prevents this from being called again
        memset(searchName, 0, NETIS_MAXFNAME+1);
      }

    }


    //if the query listener was triggered
    if (pteQuery.pte_sd > 0 && FD_ISSET(pteQuery.pte_sd, &rset)){
      //recv the image
      netic_glutinit(&argc, argv);
      pte_t accept_temp;
      recv_imsg_sd = pteQuery.pte_sd;
      recv_image_td = peer_accept(pteQuery.pte_sd, &accept_temp);
      cout << pteQuery.pte_sd << endl;
      netic_recvimsg();  // Task 1
            cout << pteQuery.pte_sd << endl;

      close(pteQuery.pte_sd); //close to shut out others from respondinggg
            cout << pteQuery.pte_sd << endl;


      netic_imginit();
      
      /* start the GLUT main loop */
      glutMainLoop();
          cout << "YES\n";
    }
  }

  exit(0);
}
