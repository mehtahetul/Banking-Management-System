# Makefile - builds the server, the clients and the helper and test tools.
#   make                 build everything below
#   make bankserver      just the server
#   make bankclient      just the menu client
#   make bankserver_fast server with a 3-second login lockout (for testing)
#   make clean           delete the built programs
#
# Programs:
#   bankserver, bankclient    the real server and menu client
#   testclient                scripted test client (words on the command line)
#   seed, dump                reset the data files / print them
#   race, threadrace          lost-update demos (Day 1 and Day 3)
#   protosize                 prints the message sizes and the port
#   test_password             checks hash_password / verify_password
#
# Recipe lines start with ">" instead of the usual tab character. That is
# what the next line switches on. It avoids the "missing separator" error
# that happens when a copy-paste turns tabs into spaces.
.RECIPEPREFIX = >

CC     = gcc
CFLAGS = -Wall -Wextra -pthread -Icommon -Istorage -Iserver
LIBS   = -lcrypt

SERVER_SRC = server/server.c server/dispatcher.c server/sessions.c \
             server/handlers_common.c server/handlers_auth.c \
             server/handlers_customer.c server/handlers_employee.c \
             server/handlers_manager.c server/handlers_admin.c \
             storage/storage.c common/netio.c common/password.c

CLIENT_SRC = client/main.c client/ui.c client/menu_common.c \
             client/menu_customer.c client/menu_employee.c \
             client/menu_manager.c client/menu_admin.c \
             common/netio.c

HEADERS = common/structs.h common/protocol.h common/netio.h common/password.h \
          storage/storage.h server/server.h server/sessions.h client/client.h

PROGRAMS = bankserver bankclient testclient seed dump \
           race threadrace protosize test_password

all: $(PROGRAMS)

bankserver: $(SERVER_SRC) $(HEADERS)
> $(CC) $(CFLAGS) -o bankserver $(SERVER_SRC) $(LIBS)

bankserver_fast: $(SERVER_SRC) $(HEADERS)
> $(CC) $(CFLAGS) -DLOCKOUT_SECONDS=3 -o bankserver_fast $(SERVER_SRC) $(LIBS)

bankclient: $(CLIENT_SRC) $(HEADERS)
> $(CC) $(CFLAGS) -Iclient -o bankclient $(CLIENT_SRC)

testclient: tests/testclient.c common/netio.c $(HEADERS)
> $(CC) $(CFLAGS) -o testclient tests/testclient.c common/netio.c

seed: tests/seed.c storage/storage.c common/password.c $(HEADERS)
> $(CC) $(CFLAGS) -o seed tests/seed.c storage/storage.c common/password.c $(LIBS)

dump: tests/dump.c storage/storage.c $(HEADERS)
> $(CC) $(CFLAGS) -o dump tests/dump.c storage/storage.c

race: tests/race.c storage/storage.c $(HEADERS)
> $(CC) $(CFLAGS) -o race tests/race.c storage/storage.c

threadrace: tests/threadrace.c storage/storage.c $(HEADERS)
> $(CC) $(CFLAGS) -o threadrace tests/threadrace.c storage/storage.c

protosize: tests/protosize.c $(HEADERS)
> $(CC) $(CFLAGS) -o protosize tests/protosize.c

test_password: tests/test_password.c common/password.c $(HEADERS)
> $(CC) $(CFLAGS) -o test_password tests/test_password.c common/password.c $(LIBS)

clean:
> rm -f $(PROGRAMS) bankserver_fast

.PHONY: all clean
