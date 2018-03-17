/******************************************************************************
 * test.c
 * ------
 * Test program to test out Lab 2's network services. This application is
 * started on the server. The client will send segments to the server, which
 * will transmit these as messages to the application. The application will
 * send messages back to the server, which will pass it back to the client.
 *
 * This program simply echoes out "Got: <message>\n". Output is sent to STDERR.
 *
 * Feel free to modify this file however you want. It will not be used in
 * grading.
 *
 * To compile, do the following:
 *     gcc test.c -o test
 *
 * To run, do the following:
 *     ./ctcp -s -p [server port] -- ./test                Server Configuration
 *     ./ctcp -c localhost:[server port] -p [client port]  Client Configuration
 *
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
  /* Stores the message sent by the client. Hopefully it isn't longer than 1000
     characters, or it will be cut off. */
  char buf[1000];

  /* Loop program forever so it can keep receiving input. */
  while (1) {
    memset(buf, 0, 1000);

    /* Read input. */
    int r = read(0, buf, 1000);
    if (r > 0)
      fprintf(stderr, "Got: %s\n", buf);
  }
  return 0;
}
