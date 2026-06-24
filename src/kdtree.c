#include <math.h>
#include <raylib.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "kdtree.h"

int comp_x(const void *a, const void *b) {
    ServerBoid *boid_a = *(ServerBoid**)a;
    ServerBoid *boid_b = *(ServerBoid**)b;

    return boid_a->b.pos.x - boid_b->b.pos.x;
}

int comp_y(const void *a, const void *b) {
    ServerBoid *boid_a = *(ServerBoid**)a;
    ServerBoid *boid_b = *(ServerBoid**)b;

    return boid_a->b.pos.y - boid_b->b.pos.y;
}

KDNode* build_kdtree(ServerBoid **boids, BoidIndex boids_count, BoidIndex leaf_size, bool axis) {
    if (boids_count <= leaf_size) { // Create a leaf
        KDNode *leaf = malloc(sizeof(KDNode));
        leaf->boids_count = boids_count;
        leaf->boids = malloc(boids_count * sizeof(ServerBoid*));
        memcpy(leaf->boids, boids, boids_count * sizeof(ServerBoid*));
        leaf->l = leaf->r = leaf->parent = NULL;
        return leaf;
    }

    axis = !axis;

    qsort(boids, boids_count, sizeof(ServerBoid*), (axis == X_AXIS)? comp_y : comp_x);
    BoidIndex median_idx = boids_count / 2;
    float median = (axis == X_AXIS)? boids[median_idx]->b.pos.y : boids[median_idx]->b.pos.x;

    KDNode *node = malloc(sizeof(KDNode));
    node->axis = axis;
    node->median = median;
    node->boids = NULL;
    node->boids_count = 0;
    node->parent = NULL;

    node->l = build_kdtree(boids, median_idx, leaf_size, axis);
    node->l->parent = node;
    node->l->part = LEFT_PART;
    node->r = build_kdtree(boids + median_idx, boids_count - median_idx, leaf_size, axis);
    node->r->parent = node;
    node->r->part = RIGHT_PART;

    return node;
}

ServerBoid* find_nearest_in_kdtree_approx(KDNode *tree, Vector2 pos, Rectangle rec) {
    // Find nearest node
    KDNode *nearest_node = tree;
    Rectangle nearest_node_rec = rec;
    while (nearest_node->boids == NULL) {
        if (nearest_node->axis == X_AXIS) {
            if (pos.y <= nearest_node->median) {
                nearest_node = nearest_node->l;
                nearest_node_rec = (Rectangle){nearest_node_rec.x, nearest_node_rec.y, nearest_node_rec.width, nearest_node->median - nearest_node_rec.y};
            } else {
                nearest_node = nearest_node->r;
                nearest_node_rec = (Rectangle){nearest_node_rec.x, nearest_node->median, nearest_node_rec.width, nearest_node_rec.y + nearest_node_rec.height - nearest_node->median};
            }
        } else {
            if (pos.x <= nearest_node->median) {
                nearest_node = nearest_node->l;
                nearest_node_rec = (Rectangle){nearest_node_rec.x, nearest_node_rec.y, nearest_node->median - nearest_node_rec.x, nearest_node_rec.height};
            } else {
                nearest_node = nearest_node->r;
                nearest_node_rec = (Rectangle){nearest_node->median, nearest_node_rec.y, nearest_node_rec.x +  nearest_node_rec.width - nearest_node->median, nearest_node_rec.height};
            }
        }
    }

    // Find nearest boid
    float min_dist = INFINITY;
    ServerBoid *nearest_boid = NULL;
    for (BoidIndex i = 0; i < nearest_node->boids_count; i++) {
        ServerBoid *boid = nearest_node->boids[i];
        if (boid->is_used)
            continue;
        
        float dist = Vector2DistanceSqr(pos, boid->b.pos);
        if (dist < min_dist) {
            min_dist = dist;
            nearest_boid = boid;
        }
    }

    // Delete empty leafs
    if (nearest_boid == NULL) {
        KDNode *parent = nearest_node->parent;

        if (parent == NULL)
            return NULL;
        
        KDNode *other_leaf = (nearest_node->part == LEFT_PART)? parent->r : parent->l;
        KDNode *grand_parent = parent->parent;

        if (grand_parent == NULL) {
            if (other_leaf->boids != NULL) {
                parent->boids_count = other_leaf->boids_count;
                parent->boids = other_leaf->boids;
            }
            parent->l = other_leaf->l;
            if (parent->l != NULL)
                parent->l->parent = parent;
            parent->r = other_leaf->r;
            if (parent->r != NULL)
                parent->r->parent = parent;
            free(other_leaf);
            free(nearest_node->boids);
            free(nearest_node);

            return find_nearest_in_kdtree_approx(parent, pos, rec);
        } else {
            if (parent->part == LEFT_PART) {
                grand_parent->l = other_leaf;
                other_leaf->part = LEFT_PART;
            } else {
                grand_parent->r = other_leaf;
                other_leaf->part = RIGHT_PART;
            }
            other_leaf->parent = grand_parent;
            free(parent);
            free(nearest_node->boids);
            free(nearest_node);

            return find_nearest_in_kdtree_approx(other_leaf, pos, rec);
        }
    }

    return nearest_boid;
}

void clear_kdtree(KDNode *root) {
    if (root->boids != NULL)
        free(root->boids);
    if (root->l != NULL)
        clear_kdtree(root->l);
    if (root->r != NULL)
        clear_kdtree(root->r);
    free(root);
}
