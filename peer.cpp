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
int maxsd;                  //global for faster calculation


void
peer_usage(char *progname)
{
  fprintf(stderr, "Usage: %s [ -p peerFQDN.port ]\n", progname); 
  exit(1);
}

bool in_Table(pte_t *pte, bool useDecline, int *location){
  int LARGEST;
  if (useDecline) LARGEST = (int) peerDecline.size();
  else LARGEST = (int) pVector.size();
  for(int i = 0; i < LARGEST; i++){
    if (!useDecline && pVector[i].pte_sd == pte->pte_sd ){
      *location = i;
      return true;
    } 
    else if(useDecline && peerDecline[i].pte_sd == pte->pte_sd){
      *location = i;
      return true;
    } 
    else return false;
  }
}

int
peer_args(int argc, char *argv[], char *pname, u_short *port)
{
  char c, *p;
  extern char *optarg;

  net_assert(!pname, "peer_args: pname not allocated");
  net_assert(!port, "peer_args: port not allocated");

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
int peer_ack(int td, char type) //peer_t *peer)
{
  int err;

  int VectorSize = pVector.size();

  unsigned char byte_arr[sizeof(pmsg_t) +(VectorSize-1)*sizeof(peer_t)];

  pmsg_t This;
  pmsg_t *sendThis = &This;
  sendThis->pm_vers = PM_VERS;
  sendThis->pm_type = type;
  //if peer is null, set num peers to 0
  sendThis->pm_npeers = (u_short) VectorSize;
  //collect data for pmsg_t - first peer_t
  //if no peers, skip this step, only send pmsg_t
  if (VectorSize == 0){
    memcpy(byte_arr, sendThis, sizeof(byte_arr));
  }
  else {
    sendThis->pm_peer = pVector[0].pte_peer;
    memcpy(byte_arr, sendThis, sizeof(pmsg_t));
  }

  //copy pte_t's - start at 1 since pVector[0] will already be copied
  //only send up to 6!!!!
  for (uint i = 1; i < 6; i++){
    if (i == 1) memcpy(byte_arr+sizeof(pmsg_t), &pVector[i].pte_peer, sizeof(pte_t));
    memcpy(byte_arr+sizeof(pte_t)*i, &pVector[i].pte_peer, sizeof(pte_t));
  }

  err = send(td, &byte_arr, sizeof(byte_arr), 0);
  if (err < 0){
    perror("error acking to peer");
    abort();
  }
  
  return(err);
}

/*
 * On success, returns 0.
 * On error, terminates process.
 */
int
peer_connect(pte_t *pte)
{
  int sd;
  if ((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
    perror("opening TCP socket");
    abort();
  }

  pte->pte_sd = sd;

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
  pVector[npeers].pending = true;  //change pending

  return(0);
}  
  

int peer_recv(int td, pmsg_t *msg)
{
  size_t min_recv = 4;  //minimum to recv is 32 bits - pmsg_t w/o peer_t
  int bytes_recv = recv(td, msg, min_recv, 0);
  if (bytes_recv <= 0){
    close(td);
    return(bytes_recv);
  }

  while((uint)bytes_recv < min_recv){
    bytes_recv += recv(td, msg, min_recv-bytes_recv, 0);
  }
  //attempt to parse out the pm_npeers
  u_short peers = msg->pm_npeers;

  for (int i = 0; i < peers; i++){
    //reset bytes recvd
    bytes_recv = 0;
    pte_t tmp;
    memset(&tmp, 0, sizeof(pte_t)); //zero-out temp
    bytes_recv = recv(td, &tmp.pte_peer, sizeof(peer_t), 0); //recv into tmp
    while((uint)bytes_recv < sizeof(peer_t)){
      bytes_recv += recv(td, &(tmp.pte_peer)+bytes_recv, sizeof(peer_t)-bytes_recv, 0); //recv into tmp
    }
    tryVector.push_back(tmp); //push onto back of vector
  }

  return (sizeof(pmsg_t));
}

//returns true on receipt of WELCOME or RDIRECT
bool recv_handler(pte_t *target){
  pmsg_t msg;
  int i, err;
  struct hostent *phost;

  int location;
  //backwards
  if (in_Table(target, false, &location)) { //if already in peer table
    pVector.erase(pVector.begin()+npeers); //erase duplicate, exit
    return false;
  }
  if (in_Table(target, true, &location)) { //if already in declined table
    pVector.erase(peerVector.begin()+npeers); //erase duplicate, exit
    return false;
  }

  err = peer_recv(target->pte_sd, &msg); // Task 2: fill in the functions peer_recv() above
  net_assert((err < 0), "peer: peer_recv");
  if (err == 0) {
    // if connection closed/error by peer, remove peer table entry
    return false;
  } 

  else {
    fprintf(stderr, "Received ack from %s:%d\n", target->pte_pname, ntohs(target->pte_peer.peer_port));
    if (msg.pm_vers != PM_VERS) {
      fprintf(stderr, "unknown message version.\n");
      return false;
    } 
    else {
      if (msg.pm_npeers) {
        // if message contains a peer address, inform user of
        // the peer two hops away
        phost = gethostbyaddr((char *) &msg.pm_peer.peer_addr,
                              sizeof(struct in_addr), AF_INET);
        fprintf(stderr, "  which is peered with: %s:%d\n", 
                ((phost && phost->h_name) ? phost->h_name :
                 inet_ntoa(msg.pm_peer.peer_addr)),
                ntohs(msg.pm_peer.peer_port));
      }
      
      if (msg.pm_type == PM_RDIRECT) {
        // inform user if message is a redirection
        fprintf(stderr, "Join redirected, try to connect to the peer above.\n");
        // add to DECLINED peers
        peerDecline.push_back(*target);
      } 
    }
    return true;
  }
}

bool accept_handler(int sd, uint npeers){
  int err;
  struct hostent *phost;
    /* Peer table is not full.  Accept the peer, send a welcome
   * message.  if we are connected to another peer, also sends
   * back the peer's address+port#
   */
  int problem = peer_accept(sd, &pVector[npeers]);

  //may have off-by-one
  // error shouldn't though -- npeers is size-1
  //remove from peer table if there is a duplicate found in peer_accept
  int location;
  if (in_Table(&pte, false, &location)) { //if already in peer table
    if (pVector[location].pending){
    } //tie breaker

    else { //this is an error
      send_RDIRECT(sd, pVector[location], true);
    }
    return false;
  }

  err = peer_ack(pVector[npeers].pte_sd, PM_WELCOME);

  err = (err != sizeof(pmsg_t));
  net_assert(err, "peer: peer_ack welcome");

  /* log connection */
  /* get the host entry info on the connected host. */
  phost = gethostbyaddr((char *) &pVector[npeers].pte_peer.peer_addr,
                      sizeof(struct in_addr), AF_INET);
  strcpy(pVector[npeers].pte_pname, 
         ((phost && phost->h_name) ? phost->h_name:
          inet_ntoa(pVector[npeers].pte_peer.peer_addr)));
  
  /* inform user of new peer */
  fprintf(stderr, "Connected from peer %s:%d\n",
          pVector[npeers].pte_pname, ntohs(pVector[npeers].pte_peer.peer_port));

  target.pending = false; //set pending back to false now that a connection has been made

  return true;
}

bool send_RDIRECT(int sd, pte_t *redirected, bool acceptedPrior){
  int err;
    // Peer table full.  Accept peer, send a redirect message.
  // Task 1: the functions peer_accept() and peer_ack() you wrote
  //         must work without error in this case also.
  if (!acceptedPrior)
    peer_accept(sd, redirected);

  err = peer_ack(redirected->pte_sd, PM_RDIRECT);
                 //(npeers > 0 ? &pVector[0].pte_peer : 0));
  err = (err != sizeof(pmsg_t));
  net_assert(err, "peer: peer_ack redirect");

  /* log connection */
  /* get the host entry info on the connected host. */
  phost = gethostbyaddr((char *) redirected->pte_peer.peer_addr,
                      sizeof(struct in_addr), AF_INET);

  /* inform user of peer redirected */
  fprintf(stderr, "Peer table full: %s:%d redirected\n",
         ((phost && phost->h_name) ? phost->h_name:
          inet_ntoa(redirected->pte_peer.peer_addr)),
          ntohs(redirected->pte_peer.peer_port));

  /* closes connection */
  close(redirected->pte_sd);
}

bool connect_handler(pte_t *connect_pte, sockaddr_in *self){
  int dummy;
  if (in_Table(connect_pte, false, &dummy)) { //if already in peer table
    return false;
  }
  dummy = -1;
  if (in_Table(connect_pte, true, &dummy)) { //if already in declined table
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
  int i, err, sd;
  struct hostent *phost;                              // the FQDN of this host
  struct sockaddr_in self;                            // the address of this host

  pte_t redirected;                 // a 2-entry peer table

  //store the host's FQDN:
  char FQDN[PR_MAXFQDN];
  char *tmpFQDN = FQDN;

  // parse args, see the comments for peer_args()
  if (peer_args(argc, argv, pVector[0].pte_pname, &pVector[0].pte_peer.peer_port)) {
    peer_usage(argv[0]);
  }

  // init (the rest!) of the data
  memset((char *) &self, 0, sizeof(struct sockaddr_in));
  if (pVector.size() > 0) {
    for (i=1; i < MAXPEERS; i++) {
      // pname[i] = &pnamebuf[i*(PR_MAXFQDN+1)];
      pVector[i].pte_sd = PR_UNINIT_SD;
      pVector[i].pte_pname = NULL;
    }

  }

  /* connect to peer if in args*/
  if (pVector[0].pte_pname) {
    connect_handler(&pVector[0], &self);
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
    //find the largest socket descriptor, set to maxsd!!!

    /* set all the descriptors to select() on */
    FD_ZERO(&rset);

    FD_SET(sd, &rset);           // or the listening socket,
    for (i = 0; i < (uint)pVector.size(); i++) {
      if (pVector[i].pte_sd > 0) {
        FD_SET(pVector[i].pte_sd, &rset);  // or the peer connected sockets
      }
    }

    struct timeval timeout;
    timeout.tv_usec = 100000; timeout.tv_sec = 0;
    select(maxsd+1, &rset, NULL, NULL, &timeout);

    // peer wants to join with me
    if (FD_ISSET(sd, &rset)) {
      // a connection is made to this host at the listened to socket
      if (pVector.size() < (uint) MAXPEERS) {
        pVector.resize(pVector.size()+1); //resize to one larger
        //init pending to false
        pVector[pVector.size()-1].pending = false;
        accept_handler(sd, pVector.size()-1); //access at last element
      } 

      else {
        //send redirect
      } 
    }

    // receiving RDIRECT or WELCOME
    uint p;
    for (p = 0; p < pVector.size(); i++) {
      if (pVector[i].pte_sd > 0 && FD_ISSET(pVector[i].pte_sd, &rset)) {
        // a message arrived from a connected peer, receive it
        bool result = recv_handler(&pVector[i]); // don't know how to use result
        // must fill up table
        for (int i = 0; i < tryVector.size(); i++){
          //if the table is full
          if ((uint)pVector.size == MAXPEERS){
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
