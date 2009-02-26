#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>

#include <sys/socket.h>
#include <arpa/inet.h>

/** Warning: No real error checking */

int main(int argc, char **argv) {
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  
  struct sockaddr_in local;
  local.sin_family = AF_INET;
  local.sin_port = htons(80);
  local.sin_addr.s_addr = INADDR_ANY;
  
  bind(sock, &local, sizeof(local));
  
  printf("Listening...\n");
  
  listen(sock, 0);
  
  char* tmp = (char*) malloc(1024);
  
  size_t sz, n;
  
  struct sockaddr_in remote;
  int client;
  while(client = accept(sock, &remote, &sz))
  {
    printf("Accepted connection from %x on port %d\n", remote.sin_addr.s_addr, ntohs(remote.sin_port));
    
    // get the request
    fd_set fd;
    FD_ZERO(&fd);
    FD_SET(client, &fd);

    select(sizeof(fd) * 8, &fd, 0, 0, 0);

    while(n = recv(client, tmp, 1024, 0))
    {
      tmp[n] = 0;
      printf("Read %u bytes: %s", n, tmp);
    }
    printf("\n");
    
    // send the reply
    strcpy(tmp, "HTTP/1.1 200 OK\r\nConnection: Close\r\n\r\nHello from Pedigree, via Berkeley Sockets!");
    sz = strlen(tmp);

    send(client, tmp, sz, 0);
    
    close(client);
  }
  
  free(tmp);
  
  close(sock);
  

  /*struct sockaddr_in remote;
  remote.sin_family = AF_INET;
  remote.sin_port = htons(1337);
  remote.sin_addr.s_addr = 0x0100a8c0; // 0x6E01a8c0
  //inet_pton(AF_INET, "72.233.89.200", &remote.sin_addr);

  connect(sock, &remote, sizeof(remote));

  size_t sz, n;
  char* tmp = (char*) malloc(1024);
  strcpy(tmp, "Does it works?");
  sz = strlen(tmp);

  send(sock, tmp, sz, 0);
  printf("Data is sent\n");
  
  fd_set fd;
  FD_ZERO(&fd);
  FD_SET(sock, &fd);

  select(sizeof(fd) * 8, &fd, 0, 0, 0);

  while(n = recv(sock, tmp, 1024, 0))
  {
    tmp[n] = 0;
    printf("Read %u bytes: %s", n, tmp);
  }
  printf("\n");

  shutdown(sock, SHUT_RDWR);
  
  close(sock);
  
  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  
  remote.sin_family = AF_INET;
  remote.sin_port = htons(1337);
  remote.sin_addr.s_addr = 0x60100a8c0;
  
  connect(sock, &remote, sizeof(remote));
  
  strcpy(tmp, "UDP Test");
  sz = strlen(tmp);
  send(sock, tmp, sz, 0);
  printf("UDP data is sent\n");
  
  close(sock);
  
  free(tmp);*/

  return 0;
}
