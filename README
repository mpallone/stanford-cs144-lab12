+-----------------------------------------------------------------------------+
|                                    README                                   |
+-----------------------------------------------------------------------------+

This document describes how to build and run cTCP.


+-----------------------------------------------------------------------------+
|                                  Building                                   |
+-----------------------------------------------------------------------------+

To build, run:

  make

To clean, run:

  make clean


+-----------------------------------------------------------------------------+
|                               Running cTCP                                  |
+-----------------------------------------------------------------------------+

Running a Server with a Single Client
-------------------------------------
To run a client at port 9999 that connects to a server on the same machine at
port 8888, first run the command to start the server:

    sudo ./ctcp -s -p 8888

Then start the client:

    sudo ./ctcp -p 9999 -c localhost:8888

You can connect many clients to the server. The server will output all of the
received data from the clients to the same STDOUT (resulting in interleaved
output). If the server replies to the clients, it will send only to the most
recently connected client.


Larger Window Sizes
-------------------
To run a client with a window size multiple of 2
(2 * MAX_SEG_DATA_SIZE = 2880 bytes), do the following:

    sudo ./ctcp -p 9999 -c localhost:8888 -w 2


Connecting to a Web Server
--------------------------
You can also run a client at port 9999 that connects to a web server at Google.
Note that you usually need to prepend the website with "www", and web servers
are generally found at port 80:

    sudo ./ctcp -p 9999 -c www.google.com:80

After you have connected, you can get the homepage by typing in the following.
Make sure you press enter after each line (e.g. after "GET / HTTP/1.1" you press
enter so "Host: .." is on a new line. That means you should not copy and paste
this but actually type it into the console).

    GET / HTTP/1.1
    Host: www.google.com
    [press enter]

Or, maybe you want to get the services page:

    GET /services/ HTTP/1.1
    Host: www.google.com
    [press enter]

Some sites we suggest testing with include google.com and bing.com. This may
not work on many websites since they require more complicated HTTP headers.

Note: If you connect to a server with e.g. port 10000, disconnect, and try to
reconnect again, it may not work for various reasons. Try connecting again with
a port number you have not used yet or waiting for a few seconds.


Running Server with Application and Multiple Clients (Lab 2)
-----------------------------------------------------------
To run a server that also runs an application, run the following command.
Each client that connects to the server will be associated with one instance
of the application (refer to the diagram for Lab 2).

    sudo ./ctcp -s -p 9999 -- sh

The server will start a new instance of the application each time a new client
connects. For example, to start two clients, which will start two instances
of an application on the server, do:

    sudo ./ctcp -c localhost:9999 -p 10000
    sudo ./ctcp -c localhost:9999 -p 10001

To run a server that runs an application with arguments, run the following
command:

    sudo ./ctcp -s -p 9999 -- grep --line-buffered "hello"
    sudo ./ctcp -s -p 9999 -- ./test

Any arguments after -- will be treated as application arguments, the first
one being the application name.

*Fun Fact* grep needs to be run with the --line-buffered flag because it
queues up its input until an EOF is read. With this flag, it can respond
after every newline.


Unreliability
-------------

The following flags can be used to create an unreliable stream:

  --drop <drop percentage>
  --corrupt <corrupt percentage>
  --delay <delay percentage>
  --duplicate <duplicate percentage>

This drops 50% of all segments coming out from this host. Unreliability must
be started on both hosts if desired from both ends.

  sudo ./ctcp -c localhost:9999 -p 12345 --drop 50



Large Binary Files
------------------
MAKE SURE you use these options carefully as they will overwrite the contents
of the file.

ctcp-client1> sudo ./ctcp [options] > newly_created_test_binary
ctcp-client2> sudo ./ctcp [options] < original_binary
