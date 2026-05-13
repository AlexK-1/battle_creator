#include <math.h>
#include <raylib.h>
#include <raymath.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "kdtree.h"
#include "boids.h"

int compX(const void *a, const void *b) {
    Boid *boidA = *(Boid**)a;
    Boid *boidB = *(Boid**)b;

    return boidA->pos.x - boidB->pos.x;
}

int compY(const void *a, const void *b) {
    Boid *boidA = *(Boid**)a;
    Boid *boidB = *(Boid**)b;

    return boidA->pos.y - boidB->pos.y;
}

KDNode* BuildKDTree(Boid **boids, BoidIndex boidsCount, BoidIndex leafSize, bool axis) {
    if (boidsCount <= leafSize) { // Create a leaf
        KDNode *leaf = malloc(sizeof(KDNode));
        leaf->boidsCount = boidsCount;
        leaf->boids = malloc(boidsCount * sizeof(Boid*));
        memcpy(leaf->boids, boids, boidsCount * sizeof(Boid*));
        leaf->l = leaf->r = leaf->parent = NULL;
        return leaf;
    }

    axis = !axis;

    qsort(boids, boidsCount, sizeof(Boid*), (axis == X_AXIS)? compY : compX);
    BoidIndex medianIdx = boidsCount / 2;
    float median = (axis == X_AXIS)? boids[medianIdx]->pos.y : boids[medianIdx]->pos.x;

    KDNode *node = malloc(sizeof(KDNode));
    node->axis = axis;
    node->median = median;
    node->boids = NULL;
    node->boidsCount = 0;
    node->parent = NULL;

    node->l = BuildKDTree(boids, medianIdx, leafSize, axis);
    node->l->parent = node;
    node->l->part = LEFT_PART;
    node->r = BuildKDTree(boids + medianIdx, boidsCount - medianIdx, leafSize, axis);
    node->r->parent = node;
    node->r->part = RIGHT_PART;

    return node;
}

Boid* FindNearestInKDTreeApprox(KDNode *tree, Vector2 pos, Rectangle rec) {
    //Find nearest node
    KDNode *nearestNode = tree;
    Rectangle nearestNodeRec = rec;
    while (nearestNode->boids == NULL) {
        if (nearestNode->axis == X_AXIS) {
            if (pos.y <= nearestNode->median) {
                nearestNode = nearestNode->l;
                nearestNodeRec = (Rectangle){nearestNodeRec.x, nearestNodeRec.y, nearestNodeRec.width, nearestNode->median - nearestNodeRec.y};
            } else {
                nearestNode = nearestNode->r;
                nearestNodeRec = (Rectangle){nearestNodeRec.x, nearestNode->median, nearestNodeRec.width, nearestNodeRec.y + nearestNodeRec.height - nearestNode->median};
            }
        } else {
            if (pos.x <= nearestNode->median) {
                nearestNode = nearestNode->l;
                nearestNodeRec = (Rectangle){nearestNodeRec.x, nearestNodeRec.y, nearestNode->median - nearestNodeRec.x, nearestNodeRec.height};
            } else {
                nearestNode = nearestNode->r;
                nearestNodeRec = (Rectangle){nearestNode->median, nearestNodeRec.y, nearestNodeRec.x +  nearestNodeRec.width - nearestNode->median, nearestNodeRec.height};
            }
        }
    }

    // Find nearest boid
    float minDist = INFINITY;
    Boid *nearestBoid = NULL;
    for (BoidIndex i = 0; i < nearestNode->boidsCount; i++) {
        Boid *boid = nearestNode->boids[i];
        if (boid->isUsed)
            continue;
        
        float dist = Vector2DistanceSqr(pos, boid->pos);
        if (dist < minDist) {
            minDist = dist;
            nearestBoid = boid;
        }
    }

    // Delete empty leafs
    if (nearestBoid == NULL) {
        KDNode *parent = nearestNode->parent;

        if (parent == NULL)
            return NULL;
        
        KDNode *otherLeaf = (nearestNode->part == LEFT_PART)? parent->r : parent->l;
        KDNode *grandParent = parent->parent;

        if (grandParent == NULL) {
            if (otherLeaf->boids != NULL) {
                parent->boidsCount = otherLeaf->boidsCount;
                parent->boids = otherLeaf->boids;
            }
            parent->l = otherLeaf->l;
            if (parent->l != NULL)
                parent->l->parent = parent;
            parent->r = otherLeaf->r;
            if (parent->r != NULL)
                parent->r->parent = parent;
            free(otherLeaf);
            free(nearestNode->boids);
            free(nearestNode);

            return FindNearestInKDTreeApprox(parent, pos, rec);
        } else {
            if (parent->part == LEFT_PART) {
                grandParent->l = otherLeaf;
                otherLeaf->part = LEFT_PART;
            } else {
                grandParent->r = otherLeaf;
                otherLeaf->part = RIGHT_PART;
            }
            otherLeaf->parent = grandParent;
            free(parent);
            free(nearestNode->boids);
            free(nearestNode);

            return FindNearestInKDTreeApprox(otherLeaf, pos, rec);
        }
    }

    return nearestBoid;
}

void ClearKDTree(KDNode *root) {
    if (root->boids != NULL)
        free(root->boids);
    if (root->l != NULL)
        ClearKDTree(root->l);
    if (root->r != NULL)
        ClearKDTree(root->r);
    free(root);
}
