#include <algorithm>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mocks/fake_mod_api.hpp"
#include "mod/mod_api.hpp"
#include "mth/core/game/component_types.hpp"
#include "mth/features/scene_gates/scene_walk.hpp"

namespace
{

constexpr std::uint64_t kTarget = 0xAAAA'0000'0000'0001ULL;
constexpr std::uint64_t kOther = 0xBBBB'0000'0000'0002ULL;

// mod_size 0 is the documented test path through looks_like_component: with no module range published
// there is nothing to range-check a vtable against, so the pointer checks stand alone.
struct Walker
{
    std::vector<void *> pending;
    std::vector<void *> buffer;

    template <typename V> mth::SceneWalk run(void *root, V &&v)
    {
        return mth::walk_scene(root, 0, 0, pending, buffer, std::forward<V>(v));
    }
};

} // namespace

TEST_CASE("scene walk: descends entities and reports leaves to the visitor, from any root", "[scene]")
{
    mth::test::ScopedFakeApi scoped;
    mth::test::fake_scene().reset();
    auto &s = mth::test::fake_scene();

    void *root = s.add(mth::rtti::kYcEntity, nullptr);
    void *child_entity = s.add(mth::rtti::kYcEntity, root);
    void *shallow = s.add(kTarget, root);
    void *deep = s.add(kTarget, child_entity);
    s.add(kOther, child_entity);

    std::vector<void *> found;
    Walker w;
    const mth::SceneWalk r = w.run(root,
                                   [&](std::span<void *const> children)
                                   {
                                       for (void *c : children)
                                           if (mod::component_isa(c, kTarget))
                                               found.push_back(c);
                                       return true;
                                   });

    REQUIRE(found.size() == 2);
    REQUIRE((found[0] == shallow || found[0] == deep));
    REQUIRE((found[1] == shallow || found[1] == deep));
    REQUIRE(found[0] != found[1]);
    REQUIRE(r.visited > 0);
    REQUIRE_FALSE(r.stopped_by_visitor);
    REQUIRE_FALSE(r.node_budget_spent);

    void *childless = s.add(mth::rtti::kYcEntity, nullptr);
    int calls = 0;
    const auto count = [&](std::span<void *const>)
    {
        ++calls;
        return true;
    };
    REQUIRE(w.run(childless, count).visited == 0);
    REQUIRE(w.run(nullptr, count).visited == 0);
    REQUIRE(calls == 0);
}

TEST_CASE("scene walk: a visitor can stop the walk, and a node wider than the child cap is walked as a prefix", "[scene]")
{
    mth::test::ScopedFakeApi scoped;
    mth::test::fake_scene().reset();
    auto &s = mth::test::fake_scene();

    void *root = s.add(mth::rtti::kYcEntity, nullptr);
    void *branch = s.add(mth::rtti::kYcEntity, root);
    s.add(kTarget, root);
    s.add(kTarget, branch);

    int calls = 0;
    Walker w;
    const mth::SceneWalk stopped = w.run(root,
                                         [&](std::span<void *const>)
                                         {
                                             ++calls;
                                             return false;
                                         });
    REQUIRE(calls == 1);
    REQUIRE(stopped.stopped_by_visitor);

    void *wide_root = s.add(mth::rtti::kYcEntity, nullptr);
    for (std::size_t i = 0; i < mth::kSceneMaxChildren + 7; ++i)
        s.add(kOther, wide_root);
    std::size_t seen = 0;
    Walker fresh;
    const mth::SceneWalk wide = fresh.run(wide_root,
                                          [&](std::span<void *const> children)
                                          {
                                              seen += children.size();
                                              return true;
                                          });
    REQUIRE(seen == mth::kSceneMaxChildren);
    REQUIRE(wide.widest_node == wide_root);
}

// ComponentIsa is a hierarchy test, so one object can be a ycEntity and the searched type. Such a node is descended into and withheld from the
// visitor, so a search misses a hit on an entity.
TEST_CASE("scene walk: a node that is also an entity is descended into and withheld from the visitor", "[scene]")
{
    mth::test::ScopedFakeApi scoped;
    mth::test::fake_scene().reset();
    auto &s = mth::test::fake_scene();

    void *root = s.add(mth::rtti::kYcEntity, nullptr);
    void *hybrid = s.add(kTarget, root, mth::rtti::kYcEntity); // isa kTarget and isa ycEntity both
    void *below = s.add(kOther, hybrid);

    std::vector<void *> seen;
    Walker w;
    w.run(root,
          [&](std::span<void *const> children)
          {
              seen.insert(seen.end(), children.begin(), children.end());
              return true;
          });

    REQUIRE(std::find(seen.begin(), seen.end(), hybrid) == seen.end());
    REQUIRE(std::find(seen.begin(), seen.end(), below) != seen.end());
}
