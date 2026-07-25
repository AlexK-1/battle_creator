#include <raylib.h>
#include <stdbool.h>

#include "boids.h"

#ifndef KDTREE_H
#define KDTREE_H

#define X_AXIS 1
#define Y_AXIS 0

#define LEFT_PART 1
#define RIGHT_PART 0

typedef struct KDNode {
    BaseBoid **boids;
    BoidIndex boids_count;
    bool axis; // true - x axis, false - y axis
    bool part; // true - left part, false - right part
    float median;
    struct KDNode *l, *r, *parent;
} KDNode; // Node/leaf of k-d tree

KDNode* build_kdtree(BaseBoid **boids, BoidIndex boids_count, BoidIndex leaf_size, bool axis);
BaseBoid* find_nearest_in_kdtree_approx(KDNode *tree, Vector2 pos, Rectangle rec);
void clear_kdtree(KDNode *root);

#define CREATE_KDTREE(boids, boids_count, leaf_size) build_kdtree(boids, boids_count, leaf_size, Y_AXIS)

#endif // KDTREE_H
