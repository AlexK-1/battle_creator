# Battle creator

A small online sandbox game. In it, you can put little men who will fight each other.
From 2 to 4 people can connect to the same game room and control their teams' boids.

## Boids

The artificial intelligence of the little men is based on the [**simulation of boids**](https://en.wikipedia.org/wiki/Boids).
They push off from each other, gather in groups and try to go in the same direction, but they have additional _modes_ of action:
* Attack: the boid is heading towards the nearest enemy
* Retreat: the boid runs away from enemies close to it
* Stop: the boid stands still until the enemy approaches it

All boids are divided into 4 teams: red, blue, green and yellow

Bots can switch their own modes of action depending on the situation, or you can change it manually.

## Client side

Since this game is online, there are 2 programs: server and client. To start the game, the user with the client program must create
a game room on the server and other players can join it. You can use this command to create a room:
```sh
./client new -s <server_ip> -n <username> -w <world_size> -p <players_number> <boids_count>
```
for example (create a new room at the 127.0.0.1 server with 'creator' username, world 1050x1050 pixels, 2 players in the room,
red and blue teams will have 30 boids):
```sh
./client new -s 127.0.0.1 -n creator -w 1050x1050 -p 2 r:b:30
````

When the server creates a room, you will receive a message with information about new room, including the _room id_,
which you must copy and send to other players. Other players can now join this room using this command:
```sh
./client join -s <server_ip> -n <username> <room_id>
```
Example:
```sh
./client join -s 127.0.0.1 -n joiner 0100f2
```

When someone tries to join your (creator's) room, you can accept them with specific team (red, blue, green or yellow) or reject them.
While other players are connecting, the *areas mode* is activated, and the creator can draw areas on the map by
clicking and dragging the right mouse button and change areas color using the keys `Q`, `W`, `E`, and `R`.
Areas mean where players will be able to place their boids.
When the required number of player is in the room, creator can press `Enter` to start placing boids.

All players places their boids on areas of their team's color using clicking the right mouse button. When all boids are placed and the player is ready,
he must press `Enter`.
When all players are ready, the game will begin!

You can press `Space` to write a message, then press `Enter` to send it to the room's chat.

You can run various commands to control the game or get information by pressing `/` key.
Some of them are:
  - `/copyroomid`, `/cri` - copy ID of the room to the clipboard
  - `/kick`, `/k @<NAME>` - kick the player out of the room (example: `/kick @user`)
  - `/changeteam`, `/ct @<NAME> <TEAM>` - change player's team (example `/ct @user blue`)
  - `/swapteams`, `/st @<>NAME1> @<NAME2>` - Swap teams of two players
For more information, see the `/help` command.

### Control

Global game control:
  - Left mouse button - move camera
  - Mouse wheel or `+`/`-` - zoom in/out
  - `L` - show/hide log
  - `Space` - write a message to the room's chat
  - `/` - write and execute command

Stage/mode-specific control:
1. Connecting/areas stage
    - creator (areas mode):
      - `Q`, `W`, `E`, `R` - change area color to red, blue, green or yellow
      - `Z` - change mode to area deletion
      - `K` - show/hide grid
      - Right mouse button - drawing rectangle area
      - When a new player is trying to joint to the room, press `Space`, type a message, then `Enter`
      - `Enter` - start placing
    - joiner (wait mode): can't do anything
2. Spawn boids stage
    - all players (spawn, select and delete modes):
      - `A` - switch to *spawn mode*; the right mouse button creates new boids
      - `S` - switch to *select mode*; the right mouse button (you can use it with the `Shift` key) highlights the boids;
        pressing the `X` key removes the boids
      - `D` - switch to *delete mode*; the right mouse button deletes the boids
      - `P`/`O` - zoom in/out the brush to create or delete boids
      - `K` - show/hide grid
      - `Enter` - you are ready
3. Game stage:
    - all players (select, direction, point and line modes):
      - `N` - Enable/disable auto-selection (if enabled, you will automatically return to select mode after using direction, point or line mode)
      - `M` - change server TPS display mode
      - `S` - switch to *select mode*; the right mouse button (you can use it with the `Shift` key) highlights the boids;
        pressing the `X` key removes the boids, and `Z` removes all orders for the movement of the boids (which are set in the following modes)
      - `D` - switch to *direction mode*; the right mouse button draws an arrow in the direction in which the selected boids will move
      - `F` - switch to *point mode*; the boids go to the point indicated on the map using the right mouse button
      - `G` - switch to *line mode*; clicking the right mouse button creates points of a polyline along which the selected boids will line up
        after pressing the `T` key

### Local game mode

You can run the game in local mode and not depend on any server by running this command:
```sh
./client local
```

In the local mode, the simulation runs with greater accuracy and better response speed than in the multiplayer mode.
But due to the higher accuracy, the load on the processor can incrases.

Control:
  - Left mouse button - move camera
  - Mouse wheel or `+`/`-` - zoom in/out
  - `N` - Enable/disable auto-selection (if enabled, you will automatically return to select mode after using direction, point or line mode)
  - `A` - switch to *spawn mode*; the right mouse button creates new boids;
    you can define the mode of action of new boids using the keys `1`, `2`, and `3` and their team using the keys `Q`, `W`, `E`, and `R`
  - `S` - switch to *select modet*; the right mouse button (you can use it with the `Shift` key) highlights the boids;
    you can select only the boids of a specific team using the keys `Q`, `W`, `E`, and `R`;
    pressing the `X` key removes the boids, and `Z` removes all orders for the movement of the boids (which are set in the following modes)
  - `D` - switch to *direction mode*; the right mouse button draws an arrow in the direction in which the selected boids will move
  - `F` - switch to *point mode*; the boids go to the point indicated on the map using the right mouse button
  - `G` - switch to *line mode*; clicking the right mouse button creates points of a polyline along which the selected boids will line up
    after pressing the `T` key

> [!TIP]
> If the game is lagging, you can try to run it with the -c flag with the chunk size value less than the default 1050 (for example: `./client local -c 300`)

### GUI

You can launch the game without any command-line arguments, and then it will open in the main menu.
From the main menu you can navigate to other menus that replicate the functionality os CLI.

I hope that interface is simple enough to understand without additional documentation, so I won't describe it here.

## Server side

The server accepts connections, process data from clients, creates rooms and launches the game.
You can start the server using this command:
```sh
./server
```

The server will output logs about main events. You can enter the `quit` or `q` command on the server to shut it down.

## Build

I decided not to use make and wrote my build system in C. So, before building server/client you must firstly compile build.c:
```sh
cc build.c -o build
```

Then build the program itself
```sh
./build server
./build client
```
or
```sh
./build all # build both server and client
```

> [!NOTE]
> The client requires [**Raylib**](https://www.raylib.com) installed

You can define macros in `build.c` for customizing build process:
  - `CC` - C Compiler (default: `"cc"`)
  - `DEBUG` - Use debugging flags
  - `USE_WAYLAND_DISPLAY` - Build with Wayland support instead of X11 on Linux (client only)
  - `RAYLIB_PATH` - Path to the Raylib repository (client only)

For Example:
```sh
cc build.c -o build -DCC=\"tcc\" -DDEBUG -DUSE_WAYLAND_DISPLAY -DRAYLIB_PATH=\"~/raylib\"
./build all
```
