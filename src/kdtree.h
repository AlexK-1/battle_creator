#include <raylib.h>
#include <stdbool.h>

#include "boids.h"

#ifndef KDTREE_H
#define KDTREE_H

#define X_AXIS true
#define Y_AXIS false

#define LEFT_PART true
#define RIGHT_PART false

typedef struct KDNode {
    Boid **boids;
    BoidIndex boidsCount;
    bool axis; // true - x axis, false - y axis
    bool part; // true - left part, false - right part
    float median;
    struct KDNode *l, *r, *parent;
} KDNode;

KDNode* BuildKDTree(Boid **boids, BoidIndex boidsCount, BoidIndex leafSize, bool axis);
Boid* FindNearestInKDTreeApprox(KDNode *tree, Vector2 pos, Rectangle rec);
void ClearKDTree(KDNode *root);

#define CreateKDTree(boids, boidsCount, leafSize) BuildKDTree(boids, boidsCount, leafSize, Y_AXIS)

#endif // KDTREE_H
