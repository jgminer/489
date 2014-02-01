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

#define net_assert(err, errmsg) { if ((err)) { perror(errmsg); assert(!(err)); } }

#define PR_PORTSEP   ':'
#define PR_UNINIT_SD  -1
#define PR_MAXFQDN   255
#define PR_QLEN      10
#define PR_LINGER    2

#define PM_VERS      0x1
#define PM_WELCOME   0x1       // Welcome peer
#define PM_RDIRECT   0x2       // Redirect per

typedef struct {            // peer address structure
  struct in_addr peer_addr; // IPv4 address
  u_short peer_port;        // port#, always stored in network byte order
  u_short peer_rsvd;        // reserved field
} peer_t;

// Message format:              8 bit  8 bit     16 bit
typedef struct {            // +------+------+-------------+
  char pm_vers, pm_type;    // | vers | type |   #peers    |
  u_short pm_npeers;        // +------+------+-------------+
  peer_t pm_peer;           // |     peer ipv4 address     |
} pmsg_t;                   // +---------------------------+
                            // |  peer port# |   reserved  |
                            // +---------------------------+

typedef struct {            // peer table entry
  int pte_sd;               // socket peer is connected at
  char *pte_pname;          // peer's fqdn
  peer_t pte_peer;          // peer's address+port#
  bool pending;             // true if waiting for ack from peer
} pte_t;                    // ptbl entry

std::vector<pte_t> pVector;     //main peer table for this host
std::vector<pte_t> tryVector;   //vector of received pte's to try
std::vector<pte_t> peerDecline; //peers recvd redirect
int MAXPEERS = 6;           //default = 6
int MAXSEND = 6;            //always 6
int maxsd = 0;                  //global for faster calculation


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
    else return false;
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

  while ((c = getopt(argc, argv, "p:n::")) != EOF) {
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
      MAXPEERS = (int)optarg; //change from 6 if no value given
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

  /* Task 1: YOUR CODE HERE
   * Fill out the rest of this function.
   */
  /* create a TCP socket, store the socket descriptor in "sd" */
  //compiled using  install g++-multilib
  int sd;
  if ((sd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)) < 0) {
    perror("socket");
    printf("Failed to create socket\n");
    abort();
  }

  //SHOULD THIS GO HERE?
  //TODO:
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
  int test = setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  if (test < 0){
    perror("setting reuse failed");
    abort();
  }

  /* bind address to socket */
  if (bind(sd, (struct sockaddr*) &self, sizeof(self)) < 0){
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
//Don't use peer anymore - just take from the pVector...
int peer_ack(int td, char type, pte_t *sendTo) //peer_t *peer)
{
  int err;
  int copiedBytes = 0;

  int VectorSize = pVector.size();

  unsigned char byte_arr[sizeof(pmsg_t) +(VectorSize-1)*sizeof(peer_t)];
  memset(byte_arr, 0, sizeof(byte_arr));

  pmsg_t This;
  pmsg_t *sendThis = &This;
  sendThis->pm_vers = PM_VERS;
  sendThis->pm_type = type;
  //if peer is null, set num peers to 0
  sendThis->pm_npeers = (u_short) VectorSize < (u_short) MAXPEERS ? VectorSize : MAXPEERS;
  sendThis->pm_peer.peer_addr.s_addr = 0;
  sendThis->pm_peer.peer_rsvd = 0;
  sendThis->pm_peer.peer_port = 0;
  //collect data for pmsg_t - first peer_t
  //if no peers, skip this step, only send pmsg_t
  //if (VectorSize == 0){
   
  //}
  // else {
  //   if(strcmp(pVector[0].pte_pname, sendTo->pte_pname)   //if we are already peered, don't returnb
  //     && (pVector[0].pte_peer.peer_port != sendTo->pte_peer.peer_port)){
  //     sendThis->pm_peer = pVector[0].pte_peer;
  //     memcpy(byte_arr, sendThis, sizeof(pmsg_t));
  //     copiedBytes += sizeof(pmsg_t);
  //   }
  // }
  int actual_sent = 0;
  //only send up to 6!!!!
  for (uint i = 0; i < pVector.size(); i++){
    if (actual_sent >= 6) break;
    if (copiedBytes < (int)sizeof(pmsg_t)){
      if(strcmp(pVector[0].pte_pname, sendTo->pte_pname)   //if we are already peered, don't returnb
      || (pVector[0].pte_peer.peer_port != sendTo->pte_peer.peer_port)){
        sendThis->pm_peer = pVector[i].pte_peer;
        memcpy(&byte_arr[0], sendThis, sizeof(pmsg_t));
        copiedBytes += sizeof(pmsg_t);
        actual_sent++;
      }
    }
    else {

      if(strcmp(pVector[0].pte_pname, sendTo->pte_pname)   //if we are already peered, don't returnb
      || (pVector[0].pte_peer.peer_port != sendTo->pte_peer.peer_port)){
        memcpy(&byte_arr[0]+copiedBytes, &pVector[i].pte_peer, sizeof(peer_t));
        copiedBytes += sizeof(peer_t);
        actual_sent++;
      }
    }
  }
  // if (actual_sent == 0){

  // }
  //check if the number of peers we are sending is what we actually specified...

  sendThis->pm_npeers = actual_sent;
  if (actual_sent == 0) copiedBytes += sizeof(pmsg_t); //keeps it below
  memcpy(&byte_arr[0], sendThis, sizeof(pmsg_t));
  assert((uint)copiedBytes == sizeof(byte_arr));
  err = send(td, &byte_arr, sizeof(byte_arr), 0);

  //memcpy(&byte_arr[0], sendThis, sizeof(pmsg_t));
  if (err < 0){
    perror("error acking to peer");
    abort();
  }
 // byte_arr = '\0'; //null byte array for future iterations
  return(err);
}

/*
 * On success, returns 0.
 * On error, terminates process.
 */
int peer_connect(pte_t *pte){
  int sd;
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
  int test = setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
  if (test < 0){
    perror("setting reuse failed");
    abort();
  }
  /* initialize socket address with destination peer's IPv4 address and port number . */
  struct sockaddr_in sin;
  //MAY NEED THESE:
  struct hostent *host = gethostbyname(pte->pte_pname);
  unsigned int server_addr = *(unsigned long *) host->h_addr_list[0];

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = server_addr;
  sin.sin_port = pte->pte_peer.peer_port;

  /* connect to destination peer. */
  if (connect(sd, (struct sockaddr *) &sin, sizeof(sin)) != 0){
      perror("failed to connect to server");
      abort();
  }
  pVector.push_back(*pte);    //push onto the peer table!
  pte->pending = true;  //change pending

  return(0);
}

void print_peer(pte_t *p){
  struct hostent *phost;
    //determines the host name, print out port as well
  phost = gethostbyaddr((char *) &p->pte_peer.peer_addr,
                        sizeof(struct in_addr), AF_INET);
  fprintf(stderr, "  which is peered with: %s:%d\n", 
          ((phost && phost->h_name) ? phost->h_name :
           inet_ntoa(p->pte_peer.peer_addr)),
          ntohs(p->pte_peer.peer_port));
}

int peer_recv(pte_t *target)
{
  pmsg_t msg;
  size_t partial = 2+sizeof(u_short);  //minimum to recv is 32 bits - pmsg_t w/o peer_t
  int bytes_recv = recv(target->pte_sd, &msg, partial, 0);
  if (bytes_recv <= 0){
    close(target->pte_sd);
    return(bytes_recv);
  }
  while((uint)bytes_recv < partial){
    bytes_recv += recv(target->pte_sd, &msg, partial-bytes_recv, 0);
  }

  fprintf(stderr, "Received ack from %s:%d\n", target->pte_pname, ntohs(target->pte_peer.peer_port));

  if (msg.pm_vers != PM_VERS) {
      fprintf(stderr, "unknown message version.\n");
      return false;
  }

  if (msg.pm_type == PM_RDIRECT) {
    // inform user if message is a redirection
    fprintf(stderr, "Join redirected, try to connect to the peer above.\n");
    // add to DECLINED peers
    peerDecline.push_back(*target);
  }

  //attempt to parse out the pm_npeers
  u_short peers = msg.pm_npeers;

  if (!peers){
    //throw out rest of pmsg_t
    pte_t gtmp;
    int garbage_bytes = 0;
    garbage_bytes = recv(target->pte_sd, &gtmp.pte_peer, sizeof(pmsg_t), 0); //recv into tmp
    while((uint)garbage_bytes < sizeof(pmsg_t)){
      bytes_recv += recv(target->pte_sd, &(gtmp.pte_peer)+garbage_bytes, sizeof(pmsg_t)-garbage_bytes, 0); //recv into tmp
    }
  }


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
bool recv_handler(pte_t *target){
  //int i;
  int err;

  err = peer_recv(target); // Task 2: fill in the functions peer_recv() above
  net_assert((err < 0), "peer: peer_recv");
  if (err == 0) {
    // if connection closed/error by peer, remove peer table entry
    return false;
  }

  else {
    target->pending = false; ///change pending to false now that we have received an ack from the other peer.
    return true;
  }
}

bool send_RDIRECT(int sd, pte_t *redirected, bool acceptedPrior){
  int err;
  struct hostent *phost;
    // Peer table full.  Accept peer, send a redirect message.
  // Task 1: the functions peer_accept() and peer_ack() you wrote
  //         must work without error in this case also.
  if (!acceptedPrior){
    peer_accept(sd, redirected);
  }

  err = peer_ack(redirected->pte_sd, PM_RDIRECT, redirected);
                 //(npeers > 0 ? &pVector[0].pte_peer : 0));
  err = (err != sizeof(pmsg_t));
  net_assert(err, "peer: peer_ack redirect");

  /* log connection */
  /* get the host entry info on the connected host. */
  phost = gethostbyaddr((char *) &redirected->pte_peer.peer_addr,
                      sizeof(struct in_addr), AF_INET);

  /* inform user of peer redirected */
  fprintf(stderr, "Peer table full: %s:%d redirected\n",
         ((phost && phost->h_name) ? phost->h_name:
          inet_ntoa(redirected->pte_peer.peer_addr)),
          ntohs(redirected->pte_peer.peer_port));

  /* closes connection */
  close(redirected->pte_sd);
  return true;
}

bool accept_handler(int sd, uint npeers){
  int err;
  struct hostent *phost;
    /* Peer table is not full.  Accept the peer, send a welcome
   * message.  if we are connected to another peer, also sends
   * back the peer's address+port#
   */
  peer_accept(sd, &pVector[npeers]);

  //may have off-by-one error shouldn't though -- npeers is size-1
  //remove from peer table if there is a duplicate found in peer_accept

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

  err = peer_ack(pVector[npeers].pte_sd, PM_WELCOME, &pVector[npeers]);

  err = (err != sizeof(pmsg_t));
  net_assert(err, "peer: peer_ack welcome");

  /* inform user of new peer */
  fprintf(stderr, "Connected from peer %s:%d\n",
          pVector[npeers].pte_pname, ntohs(pVector[npeers].pte_peer.peer_port));

  pVector[npeers].pending = false; //insure that pending is still false.

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
  // strcpy(connect_pte->pte_pname,
  //        ((phost && phost->h_name) ? phost->h_name:
  //         inet_ntoa(connect_pte->pte_peer.peer_addr)));

  if (in_Table(connect_pte, false, &dummy, false)) { //if already in peer table
    return false;
  }
  dummy = -1;
  if (in_Table(connect_pte, true, &dummy, false)) { //if already in declined table
    return false;
  }
  struct hostent *peerhostent = gethostbyname(connect_pte->pte_pname);
  connect_pte->pte_peer.peer_addr.s_addr = (unsigned int) peerhostent->h_addr_list[0];

  /* connect to peer in pte[0] */
  peer_connect(connect_pte);  // Task 2: fill in the peer_connect() function above

  //obtain ephemeral port assigned
  socklen_t selflen = sizeof(*self);
  getsockname(connect_pte->pte_sd, (struct sockaddr*) self, &selflen);

  /* inform user of connection to peer */
  fprintf(stderr, "Connected to peer %s:%d\n", connect_pte->pte_pname,
          ntohs(connect_pte->pte_peer.peer_port));
  return true;
}



int main(int argc, char *argv[])
{
  fd_set rset;
  int i, sd;
  //int err;
  //struct hostent *phost;                              // the FQDN of this host
  struct sockaddr_in self;                            // the address of this host

  //pte_t redirected;                 // a 2-entry peer table

  //store the host's FQDN:
  char FQDN[PR_MAXFQDN];
  char *tmpFQDN = FQDN;

  // parse args, see the comments for peer_args()
  if (argc > 1){
    tryVector.resize(1);
    char tmp[PR_MAXFQDN+1]; //includes space for null
    memset(&tmp, 0, PR_MAXFQDN+1); //zeros out
    tryVector[0].pte_pname = tmp;

    tryVector[0].pte_peer.peer_port = -1;
    if (peer_args(argc, argv, tryVector[0].pte_pname, &tryVector[0].pte_peer.peer_port)) {
      peer_usage(argv[0]);
    }
  }


  // // init (the rest!) of the data
  memset((char *) &self, 0, sizeof(struct sockaddr_in));
  // if (pVector.size() > 0) {
  //   for (i=1; i < MAXPEERS; i++) {
  //     // pname[i] = &pnamebuf[i*(PR_MAXFQDN+1)];
  //     pVector[i].pte_sd = PR_UNINIT_SD;
  //     pVector[i].pte_pname = NULL;
  //   }

  // }

  /* connect to peer if in args*/
  if (argc > 1){
    if (tryVector[0].pte_pname) {
      connect_handler(&tryVector[0], &self);
    }
  }


  /* setup and listen on connection */
  sd = peer_setup(self.sin_port);  // Task 1: fill in the peer_setup() function above
  if (!self.sin_port) {

    socklen_t selflen = sizeof(self);
    if (getsockname(sd, (struct sockaddr*) &self, &selflen) < 0){
      perror("getsockname");
      abort();
    }
  }

  int err_get = gethostname(tmpFQDN, PR_MAXFQDN);
  if (err_get < 0){
    perror("trouble getting host name");
    abort();
  }

  /* inform user which port this peer is listening on */
  fprintf(stderr, "This peer address is %s:%d\n",
          tmpFQDN, ntohs(self.sin_port));

  while(1) {
    /* set all the descriptors to select() on */
    FD_ZERO(&rset);

    FD_SET(sd, &rset);           // or the listening socket,
    for (i = 0; i < (int)pVector.size(); i++) {
      if (pVector[i].pte_sd > 0) {
        FD_SET(pVector[i].pte_sd, &rset);  // or the peer connected sockets
      }
    }

    struct timeval timeout;
    timeout.tv_usec = 100000; timeout.tv_sec = 0;
    select(maxsd+10, &rset, NULL, NULL, &timeout);

    // peer wants to join with me
    if (FD_ISSET(sd, &rset)) {
      // a connection is made to this host at the listened to socket
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
        recv_handler(&pVector[p]); // don't know how to use result
        // must fill up table
        for (int i = 0; i < (int)tryVector.size(); i++){
          //if the table is full
          if ((int)pVector.size() == MAXPEERS){
            break;
          }
          //otherwise, try to connect to it
          connect_handler(&tryVector[i], &self);
        }
        //erase tryVector for next time
        tryVector.clear();
      }
    }

  }

  exit(0);
}
