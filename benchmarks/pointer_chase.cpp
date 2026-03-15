/*
 * pointer_chase.cpp — Linked-list traversal with poor spatial locality.
 *
 * Allocates nodes individually on the heap so they scatter across pages,
 * then traverses the list repeatedly.  Each node->next dereference is
 * almost guaranteed to miss in cache because nodes are not contiguous.
 *
 * Build: g++ -O2 -g -fno-omit-frame-pointer -no-pie -std=c++17 -o pointer_chase pointer_chase.cpp
 */

#include <cstdlib>
#include <cstdio>
#include <vector>
#include <numeric>
#include <algorithm>
#include <random>

struct Node {
    int value;
    Node* next;
    // Pad to 64 bytes so each node occupies exactly one cache line,
    // making every hop a guaranteed miss when nodes are non-contiguous.
    char pad[64 - sizeof(int) - sizeof(Node*)];
};

static constexpr int NUM_NODES  = 1 << 20;  // ~1M nodes ≈ 64 MB
static constexpr int ITERATIONS = 5;

/*
 * Build a linked list where the traversal order is a random permutation
 * of node indices.  This defeats any prefetcher that tries to detect
 * stride patterns.
 */
static Node* build_shuffled_list() {
    // Allocate each node separately to scatter them in the heap.
    std::vector<Node*> nodes(NUM_NODES);
    for (int i = 0; i < NUM_NODES; ++i) {
        nodes[i] = new Node{i, nullptr, {}};
    }

    // Create a random traversal order.
    std::vector<int> order(NUM_NODES);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(42);  // fixed seed for reproducibility
    std::shuffle(order.begin(), order.end(), rng);

    // Link nodes according to the shuffled order.
    for (int i = 0; i + 1 < NUM_NODES; ++i) {
        nodes[order[i]]->next = nodes[order[i + 1]];
    }
    nodes[order[NUM_NODES - 1]]->next = nullptr;

    return nodes[order[0]];
}

static long traverse(Node* head) {
    long sum = 0;
    Node* cur = head;
    while (cur) {              // <-- hotspot: every cur->next is a cache miss
        sum += cur->value;
        cur = cur->next;       // <-- pointer dereference, random address
    }
    return sum;
}

int main() {
    Node* head = build_shuffled_list();

    long total = 0;
    for (int i = 0; i < ITERATIONS; ++i) {
        total += traverse(head);
    }

    printf("sum = %ld\n", total);

    // Cleanup (not performance-critical)
    Node* cur = head;
    while (cur) {
        Node* tmp = cur;
        cur = cur->next;
        delete tmp;
    }
    return 0;
}
