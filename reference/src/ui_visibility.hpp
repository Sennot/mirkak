#pragma once

#include <cstddef>
#include <string_view>

namespace cleanfeed::ui_visibility {
    inline bool isXdbotID(std::string_view id) {
        return id.starts_with("zilko.xdbot/");
    }

    struct TraversalBudget {
        std::size_t remaining = 4096;
        std::size_t maxDepth = 64;
    };

    // Read-only tree traversal: never sort, insert or reparent scene children.
    // A false result means the caller must restore ALL deferred visibility and
    // leave this frame unfiltered, rather than freeze or partially hide UI.
    template <class Node, class Owned, class Prune, class Children, class Defer>
    bool collect(Node* node, Owned const& owned, Prune const& prune,
                 Children const& children, Defer const& defer,
                 TraversalBudget& budget, std::size_t depth = 0) {
        if (!budget.remaining || depth > budget.maxDepth) return false;
        --budget.remaining;
        if (!node || !node->isVisible()) return true;
        if (owned(node)) {
            defer(node);
            return true;
        }
        if (prune(node)) return true;
        for (auto* child : children(node)) {
            if (!collect(child, owned, prune, children, defer, budget, depth + 1)) return false;
        }
        return true;
    }

    template <class Node, class Owned, class Prune, class Children, class Defer>
    bool collect(Node* node, Owned const& owned, Prune const& prune,
                 Children const& children, Defer const& defer) {
        TraversalBudget budget;
        return collect(node, owned, prune, children, defer, budget);
    }

    template <class Node>
    bool visibleUnder(Node* node, Node* root) {
        std::size_t depth = 0;
        for (auto* current = node; current && depth++ <= 64; current = current->getParent()) {
            if (!current->isVisible()) return false;
            if (current == root) return true;
        }
        return false;
    }

    template <class Node>
    bool boundedParents(Node* node) {
        std::size_t depth = 0;
        for (auto* current = node; current; current = current->getParent()) {
            if (++depth > 64) return false;
        }
        return true;
    }
}
