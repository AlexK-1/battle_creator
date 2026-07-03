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

### Control

Global game control:
  - `Left mouse button` - move camera
  - `Mouse wheel` or `+`/'-' - zoom in/out
  - `K` - show/hide grid
  - `L` - show/hide log

Stage/mode-specific control:
1. Connecting/areas stage
    - creator (areas mode):
      - `Q`, `W`, `E`, `R` - change area color to red, blue, green or yellow
      - `Z` - change mode to area deletion
      - `Right mouse button` - drawing rectangle area
      - When a new player is trying to joint to the room, press `Space`, type a message, then `Enter`
      - `Enter` - start placing
      - joiner (wait mode): can't do anything
2. Spawn boids stage
    - all (place mode):
      - `Right mouse button` - spawn boids
      - `Enter` - you are ready
3. Game stage:
    - all (select, direction, point and line modes):
      - `S` - Select mode; the right mouse button (you can use it with the `Shift` key) highlights the boids;
        you can select only the boids of a specific team using the keys `Q`, `W`, `E`, and `R`;
        pressing the `X` key removes the boids, and `Z` removes all orders for the movement of the boids (which are set in the following modes)
      - `D` - Direction mode; the right mouse button draws an arrow in the direction in which the selected boids will move
      - `F` - Point mode; the boids go to the point indicated on the map using the right mouse button
      - `G` - Line mode; clicking the right mouse button creates points of a polyline along which the selected boids will line up after pressing the `T` key

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
