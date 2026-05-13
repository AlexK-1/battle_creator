# Battle creator

A small sandbox game. In it, you can put little men who will fight each other.

## Boids

The artificial intelligence of the little men is based on the [**simulation of boids**](https://en.wikipedia.org/wiki/Boids).
They push off from each other, gather in groups and try to go in the same direction, but they have additional _modes_ of action:
* Attack: the boid is heading towards the nearest enemy
* Retreat: the boid runs away from enemies close to it
* Stop: the boid stands still until the enemy approaches it

All boids are divided into 4 teams: red, blue, green and yellow

Bots can switch their own modes of action depending on the situation, or you can change it manually.

## Control

Pressing the left mouse button moves the camera, the mouse wheel or the `+`/`-` keys zoom in/out.
The action of pressing the right mouse button depends on the control mode.

All controls are divided into several modes that can be switched using the keys:
* `A` - Spawn; the right mouse button creates new boids;
        you can define the mode of action of new boids using the keys `1`, `2`, and `3` and their action mode using the keys `Q`, `W`, `E`, and `R`
* `S` - Select; the right mouse button (you can use it with the `Shift` key) highlights the boids;
        you can select only the boids of a specific team using the keys `Q`, `W`, `E`, and `R`;
        pressing the `X` key removes the boids, and `Z` removes all orders for the movement of the boids (which are set in the following modes)
* `D` - Direction; the right mouse button draws an arrow in the direction in which the selected boids will move
* `F` - Point; the boids go to the point indicated on the map using the right mouse button
* `G` - Line; clicking the right mouse button creates points of a polyline along which the selected boids will line up after pressing the `T` key

## Build

You can compile the code using this command:
```sh
cc src/*.c -o boids -lm -lraylib -O2
````
