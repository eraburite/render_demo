// breakout: the Breakout game from LearnOpenGL 7.in_practice/3.2d_game,
// migrated onto the render_demo framework.
//
// The reference is ~2300 lines of GL across ten translation units (game,
// level, ball, sprite renderer, particle generator, post processor, text
// renderer, resource manager, shader).  Everything it does that touches the
// GPU maps onto one of four passes here:
//
//   scene pass (offscreen, 1x or MSAA+resolve)
//     one instanced draw per *texture*: background, solid bricks, bricks,
//     paddle, six power-up kinds, ball
//     one instanced draw for the fireball particles (additive)
//   screen pass (swapchain)
//     full-screen triangle: the post-processor (none / edge-detect / invert /
//     blur + vertex shake)
//     instanced glyph quads: the HUD and the menu/win text
//
// What the migration had to change, and why:
//
//   * Per-sprite draws are impossible-ish here: a descriptor set is written
//     once when the pipeline is built (DESIGN.md 20.6), so "bind a different
//     texture" means "another pipeline".  The reference's ~50 draws therefore
//     collapse to one draw per texture via an instance buffer, in the same
//     spirit as 26.3.
//   * The reference's post-processor renders into a 4x multisampled render
//     buffer and blits it into a texture.  Vulkan has no blit-resolve here:
//     the same thing is expressed as a resolve attachment, which is why the
//     scene detail below carries two colour attachments (27.2).
//   * Audio (irrklang) is dropped -- there is no audio in the framework, and a
//     silent game is a smaller lie than a stubbed one (27.7).
//   * `rand()` is replaced by std::mt19937 with a fixed default seed so that
//     two runs produce byte-identical dumps (27.6).
//
// The whole game simulation is plain CPU code with no Vulkan in it; the only
// coupling is "build the instance buffers from the game state each frame".
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "FontAtlas.h"
#include "GLFWwindow.h"
#include "ImageDump.h"
#include "TextureLoader.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "TextureSamplerCombine.h"

#include "root_directory.h"

#include "shader/particle_frag.hpp"
#include "shader/particle_vert.hpp"
#include "shader/post_frag.hpp"
#include "shader/post_vert.hpp"
#include "shader/sprite_frag.hpp"
#include "shader/sprite_vert.hpp"
#include "shader/text2d_frag.hpp"
#include "shader/text2d_vert.hpp"

namespace {

// ---------------------------------------------------------------------------
// Reference constants (game.h / power_up.h / particle_generator.h)
// ---------------------------------------------------------------------------
constexpr uint32_t kDefaultWidth = 1280;
constexpr uint32_t kDefaultHeight = 720;
constexpr float kPlayerVelocity = 500.0f;
constexpr float kBallRadius = 12.5f;
constexpr uint32_t kParticleCount = 500;
constexpr uint32_t kSeedDefault = 20221127; // fixed so dumps are reproducible

const glm::vec2 kPlayerSize(100.0f, 20.0f);
const glm::vec2 kPowerUpSize(60.0f, 20.0f);
const glm::vec2 kPowerUpVelocity(0.0f, 150.0f);
const glm::vec2 kInitialBallVelocity(100.0f, -350.0f);

// Instance-buffer capacities.  The largest level is 13x9 = 117 bricks; the
// reference has no cap on power-ups at all (every destroyed brick may spawn up
// to six), so this is a real limit worth reporting rather than discovering as
// a clipped buffer.
constexpr uint32_t kMaxBricks = 256;
constexpr uint32_t kMaxPowerUps = 64;
constexpr uint32_t kMaxGlyphQuads = 128;
constexpr uint32_t kMaxParticleInstances = kParticleCount;
constexpr uint32_t kSpriteVertexCount = 6;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::string assetPath(const char *relative)
{
    return std::string(PROJECT_ROOT_PATH) + "/" + relative;
}

struct Rng {
    explicit Rng(uint32_t seed) : m_engine(seed) {}
    uint32_t below(uint32_t bound) { return bound == 0 ? 0u : static_cast<uint32_t>(m_engine() % bound); }
    bool hit(uint32_t chance) { return below(chance) == 0; }
    std::mt19937 m_engine;
};

// ---------------------------------------------------------------------------
// Game state (a straight port of game.h/game.cpp; no Vulkan types here)
// ---------------------------------------------------------------------------
enum class GameState { eMenu, eActive, eWin };
enum class Direction { eUp, eRight, eDown, eLeft };

struct GameObject {
    glm::vec2 position{0.0f};
    glm::vec2 size{0.0f};
    glm::vec2 velocity{0.0f};
    glm::vec3 color{1.0f};
    bool solid = false;
    bool destroyed = false;
};

struct Ball {
    glm::vec2 position{0.0f};
    glm::vec2 size{0.0f};
    glm::vec2 velocity{0.0f};
    glm::vec3 color{1.0f};
    float radius = kBallRadius;
    bool stuck = true;
    bool sticky = false;
    bool passThrough = false;

    // BallObject::Move -- keeps the ball inside the window on three sides.
    void move(float dt, float windowWidth)
    {
        if (stuck) {
            return;
        }
        position += velocity * dt;
        if (position.x <= 0.0f) {
            velocity.x = -velocity.x;
            position.x = 0.0f;
        } else if (position.x + size.x >= windowWidth) {
            velocity.x = -velocity.x;
            position.x = windowWidth - size.x;
        }
        if (position.y <= 0.0f) {
            velocity.y = -velocity.y;
            position.y = 0.0f;
        }
    }

    void reset(glm::vec2 newPosition, glm::vec2 newVelocity)
    {
        position = newPosition;
        velocity = newVelocity;
        stuck = true;
        sticky = false;
        passThrough = false;
    }
};

struct PowerUp {
    std::string type;
    float duration = 0.0f;
    bool activated = false;
    GameObject body;
};

struct Particle {
    glm::vec2 position{0.0f};
    glm::vec2 velocity{0.0f};
    glm::vec4 color{1.0f};
    float life = 0.0f;
};

// ParticleGenerator: a pool of kParticleCount particles, respawned from a
// rotating "first unused" cursor.
struct ParticlePool {
    std::array<Particle, kParticleCount> particles{};
    uint32_t lastUsed = 0;

    void respawn(Particle &particle, const GameObject &object, glm::vec2 offset, Rng &rng)
    {
        const float randomOffset = (static_cast<float>(rng.below(100)) - 50.0f) / 10.0f;
        const float rColor = 0.5f + static_cast<float>(rng.below(100)) / 100.0f;
        particle.position = object.position + randomOffset + offset;
        particle.color = glm::vec4(rColor, rColor, rColor, 1.0f);
        particle.life = 1.0f;
        particle.velocity = object.velocity * 0.1f;
    }

    uint32_t firstUnused()
    {
        for (uint32_t i = lastUsed; i < kParticleCount; ++i) {
            if (particles[i].life <= 0.0f) {
                lastUsed = i;
                return i;
            }
        }
        for (uint32_t i = 0; i < lastUsed; ++i) {
            if (particles[i].life <= 0.0f) {
                lastUsed = i;
                return i;
            }
        }
        lastUsed = 0;
        return 0;
    }

    void update(float dt, const GameObject &object, uint32_t newParticles, glm::vec2 offset, Rng &rng)
    {
        for (uint32_t i = 0; i < newParticles; ++i) {
            respawn(particles[firstUnused()], object, offset, rng);
        }
        for (Particle &particle : particles) {
            particle.life -= dt;
            if (particle.life > 0.0f) {
                // The reference *subtracts* the velocity: the trail streams
                // behind the ball instead of ahead of it.
                particle.position -= particle.velocity * dt;
                particle.color.a -= dt * 2.5f;
            }
        }
    }

    uint32_t aliveCount() const
    {
        uint32_t alive = 0;
        for (const Particle &particle : particles) {
            alive += particle.life > 0.0f ? 1u : 0u;
        }
        return alive;
    }
};

struct Level {
    std::string sourcePath;
    std::vector<GameObject> bricks;

    // GameLevel::Load + init, with one deliberate difference: a missing or
    // empty level file throws.  The reference silently leaves `Bricks` empty,
    // and an empty level reports "completed", so a wrong path turns into an
    // instant win instead of an error (see DESIGN.md 27.7).
    void load(const std::string &path, float levelWidth, float levelHeight)
    {
        sourcePath = path;
        bricks.clear();

        std::ifstream stream(path);
        if (!stream) {
            throw std::runtime_error("breakout: cannot open level file " + path);
        }
        std::vector<std::vector<uint32_t>> tileData;
        std::string line;
        while (std::getline(stream, line)) {
            std::istringstream row(line);
            std::vector<uint32_t> codes;
            uint32_t code = 0;
            while (row >> code) {
                codes.push_back(code);
            }
            tileData.push_back(codes);
        }
        if (tileData.empty() || tileData[0].empty()) {
            throw std::runtime_error("breakout: level file has no tiles: " + path);
        }

        const uint32_t height = static_cast<uint32_t>(tileData.size());
        const uint32_t width = static_cast<uint32_t>(tileData[0].size());
        const float unitWidth = levelWidth / static_cast<float>(width);
        const float unitHeight = levelHeight / static_cast<float>(height);

        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < std::min<uint32_t>(width, static_cast<uint32_t>(tileData[y].size())); ++x) {
                const uint32_t code = tileData[y][x];
                if (code == 0) {
                    continue;
                }
                GameObject brick;
                brick.position = glm::vec2(unitWidth * static_cast<float>(x), unitHeight * static_cast<float>(y));
                brick.size = glm::vec2(unitWidth, unitHeight);
                if (code == 1) {
                    brick.solid = true;
                    brick.color = glm::vec3(0.8f, 0.8f, 0.7f);
                } else {
                    switch (code) {
                    case 2: brick.color = glm::vec3(0.2f, 0.6f, 1.0f); break;
                    case 3: brick.color = glm::vec3(0.0f, 0.7f, 0.0f); break;
                    case 4: brick.color = glm::vec3(0.8f, 0.8f, 0.4f); break;
                    case 5: brick.color = glm::vec3(1.0f, 0.5f, 0.0f); break;
                    default: brick.color = glm::vec3(1.0f); break;
                    }
                }
                bricks.push_back(brick);
            }
        }
    }

    bool completed() const
    {
        for (const GameObject &brick : bricks) {
            if (!brick.solid && !brick.destroyed) {
                return false;
            }
        }
        return true;
    }

    uint32_t aliveCount() const
    {
        uint32_t alive = 0;
        for (const GameObject &brick : bricks) {
            alive += brick.destroyed ? 0u : 1u;
        }
        return alive;
    }
};

struct Input {
    bool left = false;
    bool right = false;
    bool space = false;
    bool enter = false;
    bool up = false;
    bool down = false;
};

struct CollisionInfo {
    bool hit = false;
    Direction direction = Direction::eUp;
    glm::vec2 difference{0.0f};
};

// GameObject vs GameObject, AABB/AABB (reference: CheckCollision(GameObject&, GameObject&)).
bool aabbOverlap(const GameObject &one, const GameObject &two)
{
    const bool collisionX = one.position.x + one.size.x >= two.position.x
                            && two.position.x + two.size.x >= one.position.x;
    const bool collisionY = one.position.y + one.size.y >= two.position.y
                            && two.position.y + two.size.y >= one.position.y;
    return collisionX && collisionY;
}

// Which compass direction a vector points at, by maximum dot product.
Direction vectorDirection(glm::vec2 target)
{
    static const glm::vec2 compass[] = {glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, -1.0f),
                                        glm::vec2(-1.0f, 0.0f)};
    float best = 0.0f;
    Direction bestMatch = Direction::eUp;
    for (int i = 0; i < 4; ++i) {
        const float dotProduct = glm::dot(glm::normalize(target), compass[i]);
        if (dotProduct > best) {
            best = dotProduct;
            bestMatch = static_cast<Direction>(i);
        }
    }
    return bestMatch;
}

// Circle vs AABB (reference: CheckCollision(BallObject&, GameObject&)).
CollisionInfo circleCollision(const Ball &ball, const GameObject &box)
{
    const glm::vec2 center = ball.position + glm::vec2(ball.radius);
    const glm::vec2 halfExtents = box.size * 0.5f;
    const glm::vec2 boxCenter = box.position + halfExtents;
    const glm::vec2 difference = center - boxCenter;
    const glm::vec2 clamped = glm::clamp(difference, -halfExtents, halfExtents);
    const glm::vec2 closest = boxCenter + clamped;
    const glm::vec2 delta = closest - center;

    CollisionInfo info;
    // `<` and not `<=`: at exactly radius the two touch, which is the state the
    // collision response leaves them in.
    if (glm::length(delta) < ball.radius) {
        info.hit = true;
        info.direction = vectorDirection(delta);
        info.difference = delta;
    }
    return info;
}

struct Game {
    GameState state = GameState::eMenu;
    float width = static_cast<float>(kDefaultWidth);
    float height = static_cast<float>(kDefaultHeight);
    std::vector<Level> levels;
    std::vector<PowerUp> powerUps;
    ParticlePool particles;
    uint32_t level = 0;
    uint32_t lives = 3;
    GameObject player;
    Ball ball;
    Rng rng{kSeedDefault};

    // Post-processing flags (the reference's PostProcessor::Confuse/Chaos/Shake).
    bool confuse = false;
    bool chaos = false;
    bool shake = false;
    float shakeTime = 0.0f;

    // Counters for --stats; the reference has no such thing, but they are the
    // only way to see from a log that a frame-by-frame simulation did what the
    // pixels claim.
    uint32_t bricksDestroyed = 0;
    uint32_t powerUpsSpawned = 0;
    uint32_t powerUpsCollected = 0;
    uint32_t livesLost = 0;
    uint32_t levelsCleared = 0;

    Input previousInput;

    void initialise(std::vector<Level> loadedLevels, uint32_t startLevel)
    {
        levels = std::move(loadedLevels);
        level = std::min(startLevel, static_cast<uint32_t>(levels.size() - 1));

        const glm::vec2 playerPos(width / 2.0f - kPlayerSize.x / 2.0f, height - kPlayerSize.y);
        player.position = playerPos;
        player.size = kPlayerSize;
        player.color = glm::vec3(1.0f);

        ball.radius = kBallRadius;
        ball.size = glm::vec2(ball.radius * 2.0f, ball.radius * 2.0f);
        ball.color = glm::vec3(1.0f);
        ball.reset(playerPos + glm::vec2(kPlayerSize.x / 2.0f - kBallRadius, -kBallRadius * 2.0f),
                   kInitialBallVelocity);
        state = GameState::eMenu;
    }

    // --- input -------------------------------------------------------------
    void processInput(float dt, const Input &input)
    {
        const auto pressed = [&](bool now, bool before) { return now && !before; };

        if (state == GameState::eMenu) {
            if (pressed(input.enter, previousInput.enter)) {
                state = GameState::eActive;
            }
            if (pressed(input.up, previousInput.up)) {
                level = (level + 1) % static_cast<uint32_t>(levels.size());
            }
            if (pressed(input.down, previousInput.down)) {
                level = level > 0 ? level - 1 : static_cast<uint32_t>(levels.size() - 1);
            }
        } else if (state == GameState::eWin) {
            if (input.enter) {
                chaos = false;
                state = GameState::eMenu;
            }
        }

        if (state == GameState::eActive) {
            const float velocity = kPlayerVelocity * dt;
            if (input.left && player.position.x >= 0.0f) {
                player.position.x -= velocity;
                if (ball.stuck) {
                    ball.position.x -= velocity;
                }
            }
            if (input.right && player.position.x <= width - player.size.x) {
                player.position.x += velocity;
                if (ball.stuck) {
                    ball.position.x += velocity;
                }
            }
            if (input.space) {
                ball.stuck = false;
            }
        }

        previousInput = input;
    }

    // --- simulation --------------------------------------------------------
    void update(float dt)
    {
        ball.move(dt, width);
        doCollisions();
        particles.update(dt, GameObject{ball.position, ball.size, ball.velocity}, 2,
                         glm::vec2(ball.radius / 2.0f), rng);
        updatePowerUps(dt);

        if (shakeTime > 0.0f) {
            shakeTime -= dt;
            if (shakeTime <= 0.0f) {
                shake = false;
            }
        }

        if (ball.position.y >= height) {
            --lives;
            ++livesLost;
            if (lives == 0) {
                resetLevel();
                state = GameState::eMenu;
            }
            resetPlayer();
        }

        if (state == GameState::eActive && levels[level].completed()) {
            resetLevel();
            resetPlayer();
            chaos = true;
            state = GameState::eWin;
            ++levelsCleared;
        }
    }

    void doCollisions()
    {
        for (GameObject &box : levels[level].bricks) {
            if (box.destroyed) {
                continue;
            }
            const CollisionInfo collision = circleCollision(ball, box);
            if (!collision.hit) {
                continue;
            }
            if (!box.solid) {
                box.destroyed = true;
                ++bricksDestroyed;
                spawnPowerUps(box);
            } else {
                shakeTime = 0.05f;
                shake = true;
            }
            if (ball.passThrough && !box.solid) {
                continue;
            }
            if (collision.direction == Direction::eLeft || collision.direction == Direction::eRight) {
                ball.velocity.x = -ball.velocity.x;
                const float penetration = ball.radius - std::abs(collision.difference.x);
                ball.position.x += collision.direction == Direction::eLeft ? penetration : -penetration;
            } else {
                ball.velocity.y = -ball.velocity.y;
                const float penetration = ball.radius - std::abs(collision.difference.y);
                ball.position.y += collision.direction == Direction::eUp ? -penetration : penetration;
            }
        }

        for (PowerUp &powerUp : powerUps) {
            if (powerUp.body.destroyed) {
                continue;
            }
            if (powerUp.body.position.y >= height) {
                powerUp.body.destroyed = true;
            }
            if (aabbOverlap(player, powerUp.body)) {
                activatePowerUp(powerUp);
                powerUp.body.destroyed = true;
                powerUp.activated = true;
                ++powerUpsCollected;
            }
        }

        const CollisionInfo result = circleCollision(ball, player);
        if (!ball.stuck && result.hit) {
            const float centerBoard = player.position.x + player.size.x / 2.0f;
            const float distance = (ball.position.x + ball.radius) - centerBoard;
            const float percentage = distance / (player.size.x / 2.0f);
            const float strength = 2.0f;
            const glm::vec2 oldVelocity = ball.velocity;
            ball.velocity.x = kInitialBallVelocity.x * percentage * strength;
            // Keep the total speed: the angle changes, the magnitude does not.
            ball.velocity = glm::normalize(ball.velocity) * glm::length(oldVelocity);
            ball.velocity.y = -1.0f * std::abs(ball.velocity.y);
            // A sticky paddle catches the ball again after the bounce.
            ball.stuck = ball.sticky;
        }
    }

    void spawnPowerUps(const GameObject &block)
    {
        struct SpawnSpec {
            const char *type;
            glm::vec3 color;
            float duration;
            uint32_t chance;
        };
        // Order and chances are the reference's; negative power-ups are more
        // likely on purpose (1/15 vs 1/75).
        static const SpawnSpec specs[] = {
            {"speed", glm::vec3(0.5f, 0.5f, 1.0f), 0.0f, 75},
            {"sticky", glm::vec3(1.0f, 0.5f, 1.0f), 20.0f, 75},
            {"pass-through", glm::vec3(0.5f, 1.0f, 0.5f), 10.0f, 75},
            {"pad-size-increase", glm::vec3(1.0f, 0.6f, 0.4f), 0.0f, 75},
            {"confuse", glm::vec3(1.0f, 0.3f, 0.3f), 15.0f, 15},
            {"chaos", glm::vec3(0.9f, 0.25f, 0.25f), 15.0f, 15},
        };
        for (const SpawnSpec &spec : specs) {
            if (!rng.hit(spec.chance)) {
                continue;
            }
            if (powerUps.size() >= kMaxPowerUps) {
                // Drop instead of overflowing the instance buffer; the reference
                // has no bound at all.
                return;
            }
            PowerUp powerUp;
            powerUp.type = spec.type;
            powerUp.duration = spec.duration;
            powerUp.body.position = block.position;
            powerUp.body.size = kPowerUpSize;
            powerUp.body.velocity = kPowerUpVelocity;
            powerUp.body.color = spec.color;
            powerUps.push_back(powerUp);
            ++powerUpsSpawned;
        }
    }

    bool otherPowerUpActive(const std::string &type) const
    {
        for (const PowerUp &powerUp : powerUps) {
            if (powerUp.activated && powerUp.type == type) {
                return true;
            }
        }
        return false;
    }

    void activatePowerUp(PowerUp &powerUp)
    {
        if (powerUp.type == "speed") {
            ball.velocity *= 1.2f;
        } else if (powerUp.type == "sticky") {
            ball.sticky = true;
            player.color = glm::vec3(1.0f, 0.5f, 1.0f);
        } else if (powerUp.type == "pass-through") {
            ball.passThrough = true;
            ball.color = glm::vec3(1.0f, 0.5f, 0.5f);
        } else if (powerUp.type == "pad-size-increase") {
            player.size.x += 50.0f;
        } else if (powerUp.type == "confuse") {
            if (!chaos) {
                confuse = true;
            }
        } else if (powerUp.type == "chaos") {
            if (!confuse) {
                chaos = true;
            }
        }
    }

    void updatePowerUps(float dt)
    {
        for (PowerUp &powerUp : powerUps) {
            powerUp.body.position += powerUp.body.velocity * dt;
            if (!powerUp.activated) {
                continue;
            }
            powerUp.duration -= dt;
            if (powerUp.duration > 0.0f) {
                continue;
            }
            powerUp.activated = false;
            // Only switch the effect off when no other power-up of the same
            // type is still running.
            if (powerUp.type == "sticky" && !otherPowerUpActive("sticky")) {
                ball.sticky = false;
                player.color = glm::vec3(1.0f);
            } else if (powerUp.type == "pass-through" && !otherPowerUpActive("pass-through")) {
                ball.passThrough = false;
                ball.color = glm::vec3(1.0f);
            } else if (powerUp.type == "confuse" && !otherPowerUpActive("confuse")) {
                confuse = false;
            } else if (powerUp.type == "chaos" && !otherPowerUpActive("chaos")) {
                chaos = false;
            }
        }
        powerUps.erase(std::remove_if(powerUps.begin(), powerUps.end(),
                                      [](const PowerUp &powerUp) {
                                          return powerUp.body.destroyed && !powerUp.activated;
                                      }),
                       powerUps.end());
    }

    void resetLevel()
    {
        // The reference calls `Levels[i].Load("levels/one.lvl", ...)` here --
        // no `resources/` prefix, so from the build directory the open fails,
        // the level ends up empty, and the game instantly "wins".  Using the
        // stored absolute path removes that trap.
        if (level < levels.size()) {
            levels[level].load(levels[level].sourcePath, width, height / 2.0f);
        }
        lives = 3;
    }

    void resetPlayer()
    {
        player.size = kPlayerSize;
        player.position = glm::vec2(width / 2.0f - kPlayerSize.x / 2.0f, height - kPlayerSize.y);
        ball.reset(player.position + glm::vec2(kPlayerSize.x / 2.0f - kBallRadius, -kBallRadius * 2.0f),
                   kInitialBallVelocity);
        chaos = confuse = false;
        ball.passThrough = ball.sticky = false;
        player.color = glm::vec3(1.0f);
        ball.color = glm::vec3(1.0f);
    }

    void advanceLevel()
    {
        level = (level + 1) % static_cast<uint32_t>(levels.size());
        resetLevel();
        resetPlayer();
        chaos = false;
        state = GameState::eActive;
    }
};

// ---------------------------------------------------------------------------
// Sprite kinds: one pipeline (and therefore one draw) per texture
// ---------------------------------------------------------------------------
enum class Sprite : uint32_t {
    eBackground,
    eBlockSolid,
    eBlock,
    ePaddle,
    ePowerUpSpeed,
    ePowerUpSticky,
    ePowerUpPassThrough,
    ePowerUpIncrease,
    ePowerUpConfuse,
    ePowerUpChaos,
    eBall, // must stay last: the ball is drawn after the particles
    eCount
};

struct SpriteKindSpec {
    const char *name;
    const char *texture;
    uint32_t capacity;
};

constexpr SpriteKindSpec kSpriteKinds[] = {
    {"background", "data/textures/breakout/background.jpg", 1},
    {"block_solid", "data/textures/breakout/block_solid.png", kMaxBricks},
    {"block", "data/textures/breakout/block.png", kMaxBricks},
    {"paddle", "data/textures/breakout/paddle.png", 1},
    {"powerup_speed", "data/textures/breakout/powerup_speed.png", kMaxPowerUps},
    {"powerup_sticky", "data/textures/breakout/powerup_sticky.png", kMaxPowerUps},
    {"powerup_passthrough", "data/textures/breakout/powerup_passthrough.png", kMaxPowerUps},
    {"powerup_increase", "data/textures/breakout/powerup_increase.png", kMaxPowerUps},
    {"powerup_confuse", "data/textures/breakout/powerup_confuse.png", kMaxPowerUps},
    {"powerup_chaos", "data/textures/breakout/powerup_chaos.png", kMaxPowerUps},
    {"ball", "data/textures/breakout/awesomeface.png", 1},
};
static_assert(std::size(kSpriteKinds) == static_cast<size_t>(Sprite::eCount), "sprite table and enum disagree");

Sprite powerUpSprite(const std::string &type)
{
    if (type == "speed") return Sprite::ePowerUpSpeed;
    if (type == "sticky") return Sprite::ePowerUpSticky;
    if (type == "pass-through") return Sprite::ePowerUpPassThrough;
    if (type == "pad-size-increase") return Sprite::ePowerUpIncrease;
    if (type == "confuse") return Sprite::ePowerUpConfuse;
    return Sprite::ePowerUpChaos;
}

// ---------------------------------------------------------------------------
// GPU-side data
// ---------------------------------------------------------------------------
struct CameraBlock {
    alignas(16) glm::mat4 projection{1.0f};
};
static_assert(sizeof(CameraBlock) == 64, "CameraBlock must match the std140 block");

struct SpriteInstance {
    glm::vec2 position{0.0f};
    glm::vec2 size{0.0f};
    alignas(16) glm::vec4 color{1.0f};
};
static_assert(sizeof(SpriteInstance) == 32, "SpriteInstance must be position+size(16) + color(16)");

struct ParticleInstance {
    glm::vec2 position{0.0f};
    alignas(16) glm::vec4 color{1.0f};
};
static_assert(sizeof(ParticleInstance) == 32, "ParticleInstance must be vec2 + padded vec4");

struct PostBlock {
    // x = time, y = confuse, z = chaos, w = shake
    alignas(16) glm::vec4 params{0.0f};
};
static_assert(sizeof(PostBlock) == 16, "PostBlock must match the std140 block");

struct GlyphQuad {
    alignas(16) glm::vec4 rect{0.0f};  // x0, y0 (top-left), x1, y1; +y down
    alignas(16) glm::vec4 uv{0.0f};    // u0, v0, u1, v1
    alignas(16) glm::vec4 color{1.0f}; // rgb = text colour
};
static_assert(sizeof(GlyphQuad) == 48, "GlyphQuad must be rect(16) + uv(16) + color(16)");

enum class SpriteComponent { eCamera, eInstances, eTexture };
using SpriteConfig = DescriptorComponentSpec<SpriteComponent>;
inline constexpr auto SpriteComponentTable = std::make_tuple(
    SpriteConfig{SpriteComponent::eCamera, vk::DescriptorType::eUniformBuffer, 1,
                 vk::ShaderStageFlagBits::eVertex},
    SpriteConfig{SpriteComponent::eInstances, vk::DescriptorType::eStorageBuffer, 1,
                 vk::ShaderStageFlagBits::eVertex},
    SpriteConfig{SpriteComponent::eTexture, vk::DescriptorType::eCombinedImageSampler, 1,
                 vk::ShaderStageFlagBits::eFragment});

enum class PostComponent { eParams, eScene };
using PostConfig = DescriptorComponentSpec<PostComponent>;
inline constexpr auto PostComponentTable = std::make_tuple(
    PostConfig{PostComponent::eParams, vk::DescriptorType::eUniformBuffer, 1,
               vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
    PostConfig{PostComponent::eScene, vk::DescriptorType::eCombinedImageSampler, 1,
               vk::ShaderStageFlagBits::eFragment});

enum class TextComponent { eScreen, eAtlas, eQuads };
using TextConfig = DescriptorComponentSpec<TextComponent>;
inline constexpr auto TextComponentTable = std::make_tuple(
    TextConfig{TextComponent::eScreen, vk::DescriptorType::eUniformBuffer, 1,
               vk::ShaderStageFlagBits::eVertex},
    TextConfig{TextComponent::eAtlas, vk::DescriptorType::eCombinedImageSampler, 1,
               vk::ShaderStageFlagBits::eFragment},
    TextConfig{TextComponent::eQuads, vk::DescriptorType::eStorageBuffer, 1,
               vk::ShaderStageFlagBits::eVertex});

// ---------------------------------------------------------------------------
// Scene attachment: fixed-size colour target, optionally 4x MSAA + resolve
// ---------------------------------------------------------------------------
// The reference's PostProcessor owns a 4x multisampled renderbuffer (MSFBO) and
// blits it into an FBO texture before the post-process shader samples it.
// Vulkan's equivalent is a resolve attachment, which needs *two* colour
// attachments in the same subpass.  Neither SimpleAttachmentDetail::eMSAA (a
// swapchain-sized pass that presents) nor ::eOffscreen (single-sample) can
// express that, hence this detail.
//
// It is deliberately fixed-size: a swapchain-sized pass allocates one set of
// attachments shared by all in-flight frames, and this image is read by the
// next pass, so frame N+1's write would race frame N's read.  Fixed size gives
// one image per in-flight frame, at the price of not following a resize (the
// reference's FBO does not follow it either -- see DESIGN.md 27.2).
class SceneAttachmentDetail final : public IAttachmentDetail {
public:
    SceneAttachmentDetail(vk::Format colorFormat, uint32_t width, uint32_t height,
                          vk::SampleCountFlagBits samples) :
        m_colorFormat(colorFormat), m_width(width), m_height(height), m_samples(samples)
    {
    }

    bool isSwapChainSized() const override { return false; }

    RenderPassBlueprint build(uint32_t, uint32_t, vk::SampleCountFlagBits) const override
    {
        const bool resolve = m_samples != vk::SampleCountFlagBits::e1;

        RenderPassBlueprint bp;
        bp.sampleCount = m_samples;
        bp.swapChainSized = false;
        bp.colorAttachmentCount = 1;
        bp.framebufferCount = 2; // RenderManager overwrites with framesInFlight

        vk::AttachmentDescription color{};
        color.format = m_colorFormat;
        color.samples = m_samples;
        color.loadOp = vk::AttachmentLoadOp::eClear;
        color.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        color.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        // The multisampled image is throwaway once resolved.
        color.storeOp = resolve ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore;
        color.initialLayout = vk::ImageLayout::eUndefined;
        color.finalLayout = resolve ? vk::ImageLayout::eColorAttachmentOptimal
                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
        bp.attachments.push_back(color);
        bp.clearValues.push_back(vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f})));

        SubpassBlueprint subpass;
        subpass.colorAttachments.push_back(vk::AttachmentReference(0, vk::ImageLayout::eColorAttachmentOptimal));

        if (resolve) {
            vk::AttachmentDescription resolved{};
            resolved.format = m_colorFormat;
            resolved.samples = vk::SampleCountFlagBits::e1;
            resolved.loadOp = vk::AttachmentLoadOp::eDontCare;
            resolved.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
            resolved.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
            resolved.storeOp = vk::AttachmentStoreOp::eStore;
            resolved.initialLayout = vk::ImageLayout::eUndefined;
            resolved.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            bp.attachments.push_back(resolved);
            bp.clearValues.push_back(
                vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f})));
            subpass.resolveAttachments.push_back(
                vk::AttachmentReference(1, vk::ImageLayout::eColorAttachmentOptimal));
        }

        bp.subpasses.push_back(subpass);
        // Offscreen preset: releases the colour write to FRAGMENT_SHADER /
        // SHADER_READ, which is the consumer (the post-process pass).
        bp.dependencies = makeDependencies(DependencyPreset::eOffscreen);

        OffscreenTargetSpec target;
        target.attachmentIndex = 0;
        target.format = m_colorFormat;
        target.fixedSize = true;
        target.width = m_width;
        target.height = m_height;
        bp.offscreenTargets.push_back(target);

        if (resolve) {
            // The resolve target needs a spec *of its own*.  Without one
            // BasicRenderPass gives it only COLOR_ATTACHMENT | TRANSFER_SRC, so
            // it can be copied to host memory (the image dump keeps working --
            // TRANSFER_SRC is added unconditionally) but a shader sampling it
            // reads black.  The spec's default extraUsage is eSampled, which is
            // exactly what the post-process pass needs.
            OffscreenTargetSpec resolvedTarget = target;
            resolvedTarget.attachmentIndex = 1;
            bp.offscreenTargets.push_back(resolvedTarget);
        }
        return bp;
    }

private:
    vk::Format m_colorFormat;
    uint32_t m_width;
    uint32_t m_height;
    vk::SampleCountFlagBits m_samples;
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
enum class ForcedEffect { eNone, eShake, eConfuse, eChaos };

struct Options {
    bool demo = false;
    bool dump = false;
    bool stats = false;
    bool verify = false;
    bool selfTest = false;
    bool blend = true;
    uint32_t msaa = 4;
    uint32_t dumpAfter = 120;
    // Frame budget for unattended runs; 0 means "run until the window closes",
    // which is what a human playing interactively wants.
    uint32_t frames = 0;
    uint32_t startLevel = 0;
    uint32_t seed = kSeedDefault;
    ForcedEffect effect = ForcedEffect::eNone;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("breakout: " + arg + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--demo") {
            options.demo = true;
        } else if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--stats") {
            options.stats = true;
        } else if (arg == "--verify") {
            options.verify = true;
        } else if (arg == "--self-test") {
            options.selfTest = true;
        } else if (arg == "--no-blend") {
            options.blend = false;
        } else if (arg == "--msaa") {
            options.msaa = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--dump-after") {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--frames") {
            options.frames = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--level") {
            options.startLevel = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--seed") {
            options.seed = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--effect") {
            const std::string value = next();
            if (value == "none") {
                options.effect = ForcedEffect::eNone;
            } else if (value == "shake") {
                options.effect = ForcedEffect::eShake;
            } else if (value == "confuse") {
                options.effect = ForcedEffect::eConfuse;
            } else if (value == "chaos") {
                options.effect = ForcedEffect::eChaos;
            } else {
                throw std::runtime_error("breakout: --effect expects none|shake|confuse|chaos");
            }
        } else {
            throw std::runtime_error("breakout: unknown argument " + arg);
        }
    }
    // A demo with no explicit budget still has to terminate for unattended
    // validation runs; interactive mode keeps running until the window closes.
    if (options.demo && options.frames == 0) {
        options.frames = 600;
    }
    return options;
}

// ---------------------------------------------------------------------------
// Collision self-test: the one part of the game that a dump cannot verify
// ---------------------------------------------------------------------------
int selfTestFailures = 0;

void check(bool condition, const std::string &what)
{
    std::cout << (condition ? "[ok]   " : "[FAIL] ") << what << "\n";
    if (!condition) {
        ++selfTestFailures;
    }
}

void runSelfTests()
{
    // A brick at (100,100), 50x20, so its centre is (125,110).  The ball centre
    // is position + radius, and the collision test measures the vector from the
    // ball centre to the *closest point on the brick*.
    //
    // Note the direction names: they are the reference's GL compass, where +y is
    // up.  This port draws with +y down, so `eUp` means "the closest point is
    // further down the screen than the ball", i.e. the brick is below it.  The
    // physics is identical, only the spelling is inherited (DESIGN.md 27.5).
    GameObject brick;
    brick.position = glm::vec2(100.0f, 100.0f);
    brick.size = glm::vec2(50.0f, 20.0f);

    Ball ball;
    ball.radius = kBallRadius;
    ball.size = glm::vec2(kBallRadius * 2.0f);
    ball.velocity = kInitialBallVelocity;

    // Centre (125, 92): 8 px above the brick's top edge, so it is inside the
    // radius but the centre is still outside the box.
    ball.position = glm::vec2(125.0f, 92.0f - kBallRadius);
    CollisionInfo info = circleCollision(ball, brick);
    check(info.hit && info.direction == Direction::eUp,
          "ball above the brick: closest point is below the centre (eUp)");
    check(std::abs(info.difference.y - 8.0f) < 1e-4f && std::abs(info.difference.x) < 1e-4f,
          "centre-to-closest vector is (0, 8)");
    {
        // The response for eUp moves the ball up by (radius - |difference|) and
        // flips the vertical velocity -- 12.5 - 8 = 4.5 px.
        const float penetration = ball.radius - std::abs(info.difference.y);
        check(std::abs(penetration - 4.5f) < 1e-4f, "penetration is 4.5 px");
    }

    // Centre (95, 110): 5 px left of the brick's left edge.
    ball.position = glm::vec2(95.0f - kBallRadius, 110.0f - kBallRadius);
    info = circleCollision(ball, brick);
    check(info.hit && info.direction == Direction::eRight,
          "ball left of the brick: closest point is to the right (eRight)");
    check(std::abs(info.difference.x - 5.0f) < 1e-4f, "centre-to-closest vector is (5, 0)");

    // Centre (125, 87.5): exactly one radius above the top edge.  Strictly less
    // than the radius is required, otherwise the state the response leaves the
    // ball in re-triggers on the next frame.
    ball.position = glm::vec2(125.0f, 87.5f - kBallRadius);
    info = circleCollision(ball, brick);
    check(!info.hit, "a ball exactly one radius away does not count as a hit");

    // A ball whose centre is *inside* the brick has a zero separation vector;
    // the reference gets `eUp` from max-dot == 0 and resolves with a 12.5 px
    // push.  Worth pinning down: it is what makes a fast ball escape a brick it
    // tunnelled into.
    ball.position = glm::vec2(125.0f - kBallRadius, 110.0f - kBallRadius);
    info = circleCollision(ball, brick);
    check(info.hit && glm::length(info.difference) < 1e-4f,
          "a centre inside the brick reports a hit with a zero separation vector");

    GameObject paddle;
    paddle.position = glm::vec2(600.0f, 700.0f);
    paddle.size = kPlayerSize;
    check(aabbOverlap(paddle, paddle), "a box overlaps itself");
    GameObject shifted = paddle;
    shifted.position.x += kPlayerSize.x + 1.0f;
    check(!aabbOverlap(paddle, shifted), "boxes one pixel apart do not overlap");
    shifted.position.x = paddle.position.x + kPlayerSize.x;
    check(aabbOverlap(paddle, shifted), "touching boxes overlap (>=, not >)");

    // A level whose file is missing must throw instead of silently becoming an
    // empty (already completed) level.
    bool threw = false;
    try {
        Level level;
        level.load(assetPath("data/levels/does-not-exist.lvl"), 1280.0f, 360.0f);
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, "a missing level file throws instead of producing an empty level");

    Level one;
    one.load(assetPath("data/levels/one.lvl"), 1280.0f, 360.0f);
    check(!one.bricks.empty() && !one.completed(), "one.lvl loads bricks and is not already completed");

    // 15x8 grid at 1280x360: bricks are 85.33 x 45 px, all inside the playfield.
    const float unitWidth = 1280.0f / 15.0f;
    check(std::abs(one.bricks.front().size.x - unitWidth) < 1e-3f, "bricks are level-width / columns wide");
    check(one.bricks.back().position.y + one.bricks.back().size.y <= 360.0f + 1e-3f,
          "no brick hangs below the brick field");

    // --- state machine: the win screen, the life counter, and solid bricks ---
    // These are pure Game transitions, so they are checked here rather than by
    // watching a dump: a dump cannot tell a cleared level from a paused one.
    {
        auto makeGame = [](bool brickDestroyed) {
            Level level;
            // resetLevel() reloads from sourcePath, so this must be an absolute
            // path (the same one assetPath() gives the real level loader).
            level.sourcePath = assetPath("data/levels/one.lvl");
            GameObject brick;
            brick.position = glm::vec2(10.0f, 10.0f);
            brick.size = glm::vec2(50.0f, 20.0f);
            brick.destroyed = brickDestroyed;
            level.bricks.push_back(brick);
            Game game;
            game.initialise({level}, 0);
            game.state = GameState::eActive;
            game.ball.stuck = false;
            return game;
        };

        // The last breakable brick gone -> win screen, chaos forced on after
        // resetPlayer() clears it, and the level reloaded from disk behind it.
        Game won = makeGame(true);
        won.update(1.0f / 60.0f);
        check(won.state == GameState::eWin && won.levelsCleared == 1,
              "clearing the last breakable brick enters the win state");
        check(won.chaos, "the win state turns the chaos post effect on");
        check(!won.levels[0].completed(), "the win state resets the level from disk");

        // The reference's IsCompleted() ignores solid tiles, so a level with
        // only indestructible bricks left is finished, not stuck active.
        Level solidOnly;
        GameObject solidBrick;
        solidBrick.solid = true;
        solidOnly.bricks.push_back(solidBrick);
        check(solidOnly.completed(), "indestructible bricks do not block level completion");

        // Ball below the floor -> one life lost and the ball parked on the
        // paddle again.  Put it well below so the upward velocity cannot bring
        // it back inside during the same update().
        Game lost = makeGame(false);
        lost.ball.position.y = lost.height + 100.0f;
        lost.update(1.0f / 60.0f);
        check(lost.lives == 2 && lost.livesLost == 1, "the ball falling past the floor costs one life");
        check(lost.ball.stuck && lost.ball.position.y < lost.height,
              "a lost ball is parked back on the paddle");
    }

    std::cout << (selfTestFailures == 0 ? "[self-test] all checks passed\n" : "[self-test] FAILURES: ")
              << (selfTestFailures == 0 ? "" : std::to_string(selfTestFailures) + "\n");
    if (selfTestFailures != 0) {
        throw std::runtime_error("breakout: collision self-test failed");
    }
}

// ---------------------------------------------------------------------------
// Text layout (reference: TextRenderer::RenderText, pixel space, +y down)
// ---------------------------------------------------------------------------
void layoutRun(const FontAtlas &atlas, const std::string &text, float x, float y, float scale, glm::vec3 color,
               std::vector<GlyphQuad> &out)
{
    const GlyphMetrics *reference = atlas.find('H');
    const float capHeight = reference ? reference->bearing.y : 0.0f;
    float penX = x;
    for (char character : text) {
        const GlyphMetrics *glyph = atlas.find(static_cast<uint32_t>(static_cast<unsigned char>(character)));
        if (glyph == nullptr) {
            continue;
        }
        if (glyph->size.x > 0.0f && glyph->size.y > 0.0f) {
            const float xpos = penX + glyph->bearing.x * scale;
            const float ypos = y + (capHeight - glyph->bearing.y) * scale;
            GlyphQuad quad;
            quad.rect = glm::vec4(xpos, ypos, xpos + glyph->size.x * scale, ypos + glyph->size.y * scale);
            quad.uv = glyph->uv;
            quad.color = glm::vec4(color, 1.0f);
            out.push_back(quad);
        }
        penX += glyph->advance * scale;
    }
}

// ---------------------------------------------------------------------------
// Resources owned by one sprite kind
// ---------------------------------------------------------------------------
struct SpriteKind {
    std::shared_ptr<ShaderAssetGroup> asset;
    std::shared_ptr<GraphicsPipeline> pipeline;
    std::shared_ptr<CmdOpDrawFrame> draw;
    std::shared_ptr<CmdSyncDevice> sync;
    std::vector<SpriteInstance> instances;
    uint32_t capacity = 0;
};

uint32_t sampleCountFor(vk::SampleCountFlagBits maxUsable, uint32_t requested)
{
    if (requested <= 1 || maxUsable == vk::SampleCountFlagBits::e1) {
        return 1;
    }
    const uint32_t usable = static_cast<uint32_t>(maxUsable);
    uint32_t samples = 1;
    while (samples * 2 <= requested && samples * 2 <= usable) {
        samples *= 2;
    }
    return samples;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);
    if (options.selfTest) {
        runSelfTests();
        return;
    }

    // A Camera is mandatory for the window, but a 2D game never reads it: the
    // projection is a pixel-space ortho built per frame.
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(kDefaultWidth, kDefaultHeight, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();
    const float gameWidth = static_cast<float>(extent.width);
    const float gameHeight = static_cast<float>(extent.height);

    // ----- game state -------------------------------------------------------
    std::vector<Level> levels(4);
    static const char *levelFiles[] = {"one.lvl", "two.lvl", "three.lvl", "four.lvl"};
    for (size_t i = 0; i < levels.size(); ++i) {
        levels[i].load(assetPath((std::string("data/levels/") + levelFiles[i]).c_str()), gameWidth,
                       gameHeight / 2.0f);
    }

    Game game;
    game.width = gameWidth;
    game.height = gameHeight;
    game.rng = Rng(options.seed);
    game.initialise(levels, options.startLevel);

    const FontAtlas atlas = FontAtlas::load(assetPath("data/fonts/Antonio-Bold.ttf"), 24, 32, 126, 0);

    // ----- passes -----------------------------------------------------------
    const vk::SampleCountFlagBits maxSamples = mgr->getMaxUsableSampleCount();
    uint32_t samples = sampleCountFor(maxSamples, options.msaa);
    const vk::Format sceneFormat = vk::Format::eR8G8B8A8Unorm;
    const vk::Format swapFormat = *mgr->getSwapChain()->getImageFormat();

    if (samples > 1) {
        // Cheap sanity check on the format/sample-count pair.  R8G8B8A8_UNORM is
        // the safest multi-sampled colour format (it is what every driver
        // advertises) but the scene target is *not* the swapchain, so nothing
        // guarantees it -- if the format refuses, fall back instead of letting
        // validation complain about every frame.
        vk::ImageFormatProperties formatProperties{};
        const vk::Result queryResult = mgr->getPhysicalDevice()->getImageFormatProperties(
            sceneFormat, vk::ImageType::e2D, vk::ImageTiling::eOptimal,
            vk::ImageUsageFlagBits::eColorAttachment, vk::ImageCreateFlags{}, &formatProperties);
        if (queryResult != vk::Result::eSuccess
            || !(formatProperties.sampleCounts & static_cast<vk::SampleCountFlags>(samples))) {
            std::cout << "[breakout] " << samples << "x MSAA unsupported for " << vk::to_string(sceneFormat)
                      << ", falling back to 1x\n";
            samples = 1;
        }
    }

    auto scenePass = mgr->CreateRenderPass(
        std::make_shared<SceneAttachmentDetail>(sceneFormat, extent.width, extent.height,
                                                static_cast<vk::SampleCountFlagBits>(samples)));
    scenePass->createFramebuffers({}, scenePass->extent());
    // With a resolve, only the single-sampled image is registered.
    if (scenePass->offscreenColorCountPerSet() != 1) {
        throw std::runtime_error("breakout: the scene pass must expose exactly one sampled colour target");
    }

    auto screenPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, swapFormat, vk::Format::eUndefined));

    // ----- sprite pipelines: one per texture ---------------------------------
    std::array<SpriteKind, static_cast<size_t>(Sprite::eCount)> spriteKinds;
    // The reference's Texture2D leaves the default Wrap_S/Wrap_T of GL_REPEAT, which matters for
    // the post-processor: its 3x3 kernels read uv +-1/300, so with clamp the samples outside the
    // quad smear the edge texel inwards while the reference wraps around to the opposite side.
    const vk::SamplerCreateInfo samplerInfo = []() {
        vk::SamplerCreateInfo info{};
        info.magFilter = vk::Filter::eLinear;
        info.minFilter = vk::Filter::eLinear;
        info.mipmapMode = vk::SamplerMipmapMode::eLinear;
        info.addressModeU = vk::SamplerAddressMode::eRepeat;
        info.addressModeV = vk::SamplerAddressMode::eRepeat;
        info.addressModeW = vk::SamplerAddressMode::eRepeat;
        info.maxLod = VK_LOD_CLAMP_NONE;
        return info;
    }();
    const auto sampler = std::make_shared<vk::Sampler>(mgr->getLogicalDevice()->createSampler(samplerInfo));

    for (uint32_t kind = 0; kind < static_cast<uint32_t>(Sprite::eCount); ++kind) {
        const SpriteKindSpec &spec = kSpriteKinds[kind];
        SpriteKind &target = spriteKinds[kind];
        target.capacity = spec.capacity;
        target.instances.reserve(spec.capacity);

        const LoadedTexture texture = loadTexture(assetPath(spec.texture).c_str());

        auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
        auto cameraSet = DescriptorLayoutAtlas::makeSet<SpriteComponent, SpriteComponentTable>(
            0, {SpriteComponent::eCamera}, UpdateFrequency::eEachFrame, false);
        cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraBlock)), DataTypes::eCustom};

        auto instanceSet = DescriptorLayoutAtlas::makeSet<SpriteComponent, SpriteComponentTable>(
            1, {SpriteComponent::eInstances}, UpdateFrequency::eEachFrame, false);
        instanceSet.specs[0] =
            GBufferSpec{spec.capacity, static_cast<uint32_t>(sizeof(SpriteInstance)), DataTypes::eCustom};

        auto textureSet = DescriptorLayoutAtlas::makeSet<SpriteComponent, SpriteComponentTable>(
            2, {SpriteComponent::eTexture}, UpdateFrequency::eStatic, true);
        TextureImageSpec textureSpec;
        textureSpec.width = static_cast<uint32_t>(texture.width);
        textureSpec.height = static_cast<uint32_t>(texture.height);
        textureSpec.channels = static_cast<uint32_t>(texture.channels);
        textureSpec.miplevels = texture.mipLevels;
        // sRGB: the pngs are authored in sRGB, the scene target is UNORM, and the
        // swapchain is sRGB, so exactly one decode and one encode happen.
        textureSpec.format = vk::Format::eR8G8B8A8Srgb;
        textureSet.specs[0] = textureSpec;

        layout->setDetails({cameraSet, instanceSet, textureSet});

        target.asset = mgr->CreateShaderAssetGroup(layout);
        target.asset->addShaderSpirvCode(
            "vert", std::vector<uint32_t>(shader::SPRITE_VERT_SPV.begin(), shader::SPRITE_VERT_SPV.end()));
        target.asset->addShaderSpirvCode(
            "frag", std::vector<uint32_t>(shader::SPRITE_FRAG_SPV.begin(), shader::SPRITE_FRAG_SPV.end()));

        auto vertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                             DataTypes::eUnsignedInt, true, kSpriteVertexCount, 1);
        GraphicsPipelineConfig config;
        config.depthTestEnable = false;
        config.depthWriteEnable = false;
        config.blendEnable = options.blend;
        target.pipeline = mgr->CreatePipeline(vertexInput, layout, target.asset, scenePass, 0, config);
        target.draw = std::make_shared<CmdOpDrawFrame>(target.pipeline);
        // One sync per asset: the camera UBO and the instance buffer are written
        // every frame, and only the owner asset knows which buffers are dirty.
        target.sync = std::make_shared<CmdSyncDevice>(target.asset);

        auto upload = mgr->UploadSequence();
        auto sync = std::make_shared<CmdSyncDevice>(target.asset);
        sync->OnUpdateRawData(ResourceId(2, 0), 0u, texture.pixels);
        upload->submit(sync, target.asset);
    }

    // ----- particles --------------------------------------------------------
    // Same shader pair as the sprites but a different instance record and an
    // additive blend state, so it needs its own pipeline.
    const LoadedTexture particleTexture = loadTexture(assetPath("data/textures/breakout/particle.png").c_str());
    auto particleLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto particleCameraSet = DescriptorLayoutAtlas::makeSet<SpriteComponent, SpriteComponentTable>(
        0, {SpriteComponent::eCamera}, UpdateFrequency::eEachFrame, false);
    particleCameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraBlock)), DataTypes::eCustom};
    auto particleInstanceSet = DescriptorLayoutAtlas::makeSet<SpriteComponent, SpriteComponentTable>(
        1, {SpriteComponent::eInstances}, UpdateFrequency::eEachFrame, false);
    particleInstanceSet.specs[0] =
        GBufferSpec{kMaxParticleInstances, static_cast<uint32_t>(sizeof(ParticleInstance)), DataTypes::eCustom};
    auto particleTextureSet = DescriptorLayoutAtlas::makeSet<SpriteComponent, SpriteComponentTable>(
        2, {SpriteComponent::eTexture}, UpdateFrequency::eStatic, true);
    TextureImageSpec particleTextureSpec;
    particleTextureSpec.width = static_cast<uint32_t>(particleTexture.width);
    particleTextureSpec.height = static_cast<uint32_t>(particleTexture.height);
    particleTextureSpec.channels = static_cast<uint32_t>(particleTexture.channels);
    particleTextureSpec.miplevels = particleTexture.mipLevels;
    particleTextureSpec.format = vk::Format::eR8G8B8A8Srgb;
    particleTextureSet.specs[0] = particleTextureSpec;
    particleLayout->setDetails({particleCameraSet, particleInstanceSet, particleTextureSet});

    auto particleAsset = mgr->CreateShaderAssetGroup(particleLayout);
    particleAsset->addShaderSpirvCode(
        "vert", std::vector<uint32_t>(shader::PARTICLE_VERT_SPV.begin(), shader::PARTICLE_VERT_SPV.end()));
    particleAsset->addShaderSpirvCode(
        "frag", std::vector<uint32_t>(shader::PARTICLE_FRAG_SPV.begin(), shader::PARTICLE_FRAG_SPV.end()));

    auto particleVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                                 DataTypes::eUnsignedInt, true, kSpriteVertexCount, 0);
    GraphicsPipelineConfig particleConfig;
    particleConfig.depthTestEnable = false;
    particleConfig.depthWriteEnable = false;
    particleConfig.blendAttachments = {ColorBlendAttachment::additive()};
    auto particlePipeline =
        mgr->CreatePipeline(particleVertexInput, particleLayout, particleAsset, scenePass, 0, particleConfig);
    auto particleDraw = std::make_shared<CmdOpDrawFrame>(particlePipeline);
    auto particleSync = std::make_shared<CmdSyncDevice>(particleAsset);
    {
        auto upload = mgr->UploadSequence();
        auto sync = std::make_shared<CmdSyncDevice>(particleAsset);
        sync->OnUpdateRawData(ResourceId(2, 0), 0u, particleTexture.pixels);
        upload->submit(sync, particleAsset);
    }

    // ----- post-process pipeline --------------------------------------------
    auto postLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto postParamSet = DescriptorLayoutAtlas::makeSet<PostComponent, PostComponentTable>(
        0, {PostComponent::eParams}, UpdateFrequency::eEachFrame, false);
    postParamSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(PostBlock)), DataTypes::eCustom};
    auto postSceneSet = DescriptorLayoutAtlas::makeSet<PostComponent, PostComponentTable>(
        1, {PostComponent::eScene}, UpdateFrequency::eEachFrame, false);
    postLayout->setDetails({postParamSet, postSceneSet});

    auto postAsset = mgr->CreateShaderAssetGroup(postLayout);
    postAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::POST_VERT_SPV.begin(),
                                                               shader::POST_VERT_SPV.end()));
    postAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::POST_FRAG_SPV.begin(),
                                                               shader::POST_FRAG_SPV.end()));

    // Every view the post pass will sample must be bound before the pipeline is
    // built (DESIGN.md 20.6): one resolve target per in-flight frame.
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        auto combine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(scenePass->offscreenViews()[frame], *sampler,
                                      vk::ImageLayout::eShaderReadOnlyOptimal),
            [](TextureSamplerCombine *pointer) { delete pointer; });
        postAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(1, 0), frame), combine);
    }

    auto fullscreenInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                             DataTypes::eUnsignedInt, true, 3u, 1u);
    GraphicsPipelineConfig postConfig;
    postConfig.depthTestEnable = false;
    postConfig.depthWriteEnable = false;
    auto postPipeline = mgr->CreatePipeline(fullscreenInput, postLayout, postAsset, screenPass, 0, postConfig);
    auto postDraw = std::make_shared<CmdOpDrawFrame>(postPipeline);
    auto postFrameSync = std::make_shared<CmdSyncDevice>(postAsset);

    // ----- text pipeline ----------------------------------------------------
    auto textLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto textScreenSet = DescriptorLayoutAtlas::makeSet<TextComponent, TextComponentTable>(
        0, {TextComponent::eScreen}, UpdateFrequency::eEachFrame, false);
    textScreenSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraBlock)), DataTypes::eCustom};
    auto textAtlasSet = DescriptorLayoutAtlas::makeSet<TextComponent, TextComponentTable>(
        1, {TextComponent::eAtlas}, UpdateFrequency::eStatic, true);
    TextureImageSpec atlasSpec;
    atlasSpec.width = atlas.width();
    atlasSpec.height = atlas.height();
    atlasSpec.channels = 1;
    atlasSpec.format = vk::Format::eR8Unorm; // coverage, not colour
    atlasSpec.miplevels = 1;
    atlasSpec.anisotropy = false;
    atlasSpec.addressMode = vk::SamplerAddressMode::eClampToEdge;
    textAtlasSet.specs[0] = atlasSpec;
    auto textQuadSet = DescriptorLayoutAtlas::makeSet<TextComponent, TextComponentTable>(
        2, {TextComponent::eQuads}, UpdateFrequency::eEachFrame, false);
    textQuadSet.specs[0] = GBufferSpec{kMaxGlyphQuads, static_cast<uint32_t>(sizeof(GlyphQuad)), DataTypes::eCustom};
    textLayout->setDetails({textScreenSet, textAtlasSet, textQuadSet});

    auto textAsset = mgr->CreateShaderAssetGroup(textLayout);
    textAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::TEXT2D_VERT_SPV.begin(),
                                                               shader::TEXT2D_VERT_SPV.end()));
    textAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::TEXT2D_FRAG_SPV.begin(),
                                                               shader::TEXT2D_FRAG_SPV.end()));

    auto textVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                             DataTypes::eUnsignedInt, true, kSpriteVertexCount, 0);
    GraphicsPipelineConfig textConfig;
    textConfig.depthTestEnable = false;
    textConfig.depthWriteEnable = false;
    textConfig.blendEnable = true;
    auto textPipeline = mgr->CreatePipeline(textVertexInput, textLayout, textAsset, screenPass, 0, textConfig);
    auto textDraw = std::make_shared<CmdOpDrawFrame>(textPipeline);
    auto textFrameSync = std::make_shared<CmdSyncDevice>(textAsset);
    {
        auto upload = mgr->UploadSequence();
        auto sync = std::make_shared<CmdSyncDevice>(textAsset);
        sync->OnUpdateRawData(ResourceId(1, 0), 0u, atlas.pixels());
        upload->submit(sync, textAsset);
    }

    std::cout << "[breakout] " << gameWidth << "x" << gameHeight << ", " << samples << "x MSAA, "
              << static_cast<uint32_t>(Sprite::eCount) << " sprite pipelines, "
              << (options.blend ? "alpha blending" : "blending OFF") << ", "
              << (options.demo ? "demo" : "interactive") << " mode, seed " << options.seed << "\n";
    std::cout << "[breakout] font " << atlas.width() << "x" << atlas.height() << " (" << atlas.glyphCount()
              << " glyphs), level " << game.level << " with " << game.levels[game.level].bricks.size()
              << " bricks\n";

    // ----- frame loop -------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    std::vector<GlyphQuad> glyphQuads;
    std::vector<ParticleInstance> particleInstances;

    uint32_t framesRendered = 0;
    bool dumpDone = false;
    bool verified = false;
    // The staging -> device copy happens inside the frame's own sequence, so the
    // device buffer can only be read back once that frame has been submitted.
    // The CPU array is snapshotted on the first frame and compared one frame later.
    std::vector<SpriteInstance> verifySnapshot;
    bool verifyPending = false;
    float wallClock = 0.0f;
    float simTime = 0.0f;
    uint32_t winHold = 0;

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime); // ESC closes, and drives the frame clock

        const float dt = options.demo ? 1.0f / 60.0f : std::min(std::max(currentTime - wallClock, 0.0f), 0.05f);
        wallClock = currentTime;
        simTime += dt;

        // --- input ---
        Input input;
        if (options.demo) {
            // Attract mode: the paddle tracks the ball and SPACE is held until
            // the ball is released.  Deterministic by construction -- no clock,
            // no mouse, no random goal.
            if (game.state == GameState::eActive) {
                const float target = game.ball.position.x + game.ball.size.x / 2.0f - game.player.size.x / 2.0f;
                input.left = game.player.position.x > target + 1.0f;
                input.right = game.player.position.x < target - 1.0f;
                input.space = true;
            } else if (game.state == GameState::eMenu) {
                input.enter = true;
            }
            // eWin: hold ENTER back for a second so a dump can catch the win
            // screen, then advance to the next level instead of waiting for a
            // human (which is what the reference's GAME_WIN state does).
        } else {
            input.left = window.isKeyDown(GLFW_KEY_A) || window.isKeyDown(GLFW_KEY_LEFT);
            input.right = window.isKeyDown(GLFW_KEY_D) || window.isKeyDown(GLFW_KEY_RIGHT);
            input.space = window.isKeyDown(GLFW_KEY_SPACE);
            input.enter = window.isKeyDown(GLFW_KEY_ENTER);
            input.up = window.isKeyDown(GLFW_KEY_W);
            input.down = window.isKeyDown(GLFW_KEY_S);
        }

        game.processInput(dt, input);
        if (game.state == GameState::eActive) {
            game.update(dt);
        }
        if (options.demo) {
            if (game.state == GameState::eWin) {
                if (++winHold > 60) {
                    game.advanceLevel();
                    winHold = 0;
                }
            } else {
                winHold = 0;
            }
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();

        // --- instance buffers from the game state ---
        for (SpriteKind &target : spriteKinds) {
            target.instances.clear();
        }
        const auto addSprite = [&](Sprite kind, glm::vec2 position, glm::vec2 size, glm::vec3 color) {
            SpriteInstance instance;
            instance.position = position;
            instance.size = size;
            instance.color = glm::vec4(color, 1.0f);
            spriteKinds[static_cast<size_t>(kind)].instances.push_back(instance);
        };

        // Painter's order, exactly as the reference draws: background, bricks,
        // paddle, power-ups, particles, ball.  There is no depth buffer.
        addSprite(Sprite::eBackground, glm::vec2(0.0f), glm::vec2(gameWidth, gameHeight), glm::vec3(1.0f));
        for (const GameObject &brick : game.levels[game.level].bricks) {
            if (!brick.destroyed) {
                addSprite(brick.solid ? Sprite::eBlockSolid : Sprite::eBlock, brick.position, brick.size,
                          brick.color);
            }
        }
        addSprite(Sprite::ePaddle, game.player.position, game.player.size, game.player.color);
        for (const PowerUp &powerUp : game.powerUps) {
            if (!powerUp.body.destroyed) {
                addSprite(powerUpSprite(powerUp.type), powerUp.body.position, powerUp.body.size,
                          powerUp.body.color);
            }
        }
        addSprite(Sprite::eBall, game.ball.position, game.ball.size, game.ball.color);

        // Particles: only the live ones go to the GPU, so the draw count is the
        // real particle count instead of a discard-everything 500.
        particleInstances.clear();
        for (const Particle &particle : game.particles.particles) {
            if (particle.life <= 0.0f) {
                continue;
            }
            ParticleInstance instance;
            instance.position = particle.position;
            instance.color = particle.color;
            particleInstances.push_back(instance);
        }

        // Text: rebuilt every frame because the HUD shows the lives count.
        glyphQuads.clear();
        const float uiScale = 1.0f;
        layoutRun(atlas, "Lives:" + std::to_string(game.lives), 5.0f, 5.0f, uiScale, glm::vec3(1.0f), glyphQuads);
        if (game.state == GameState::eMenu) {
            layoutRun(atlas, "Press ENTER to start", 250.0f, gameHeight / 2.0f, 1.0f, glm::vec3(1.0f), glyphQuads);
            layoutRun(atlas, "Press W or S to select level", 245.0f, gameHeight / 2.0f + 20.0f, 0.75f,
                      glm::vec3(1.0f), glyphQuads);
        } else if (game.state == GameState::eWin) {
            layoutRun(atlas, "You WON!!!", 320.0f, gameHeight / 2.0f - 20.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f),
                      glyphQuads);
            layoutRun(atlas, "Press ENTER to retry or ESC to quit", 130.0f, gameHeight / 2.0f, 1.0f,
                      glm::vec3(1.0f, 1.0f, 0.0f), glyphQuads);
        }
        if (glyphQuads.size() > kMaxGlyphQuads) {
            glyphQuads.resize(kMaxGlyphQuads);
        }

        // --- per-frame uploads ---
        CameraBlock cameraBlock;
        // Vulkan's NDC has +y pointing down, so glm::ortho(0, w, 0, h) maps
        // pixel y=0 to the top of the framebuffer: it is the reference's GL
        // ortho(0, w, h, 0) with bottom/top swapped for the flipped NDC.
        // text_rendering does the opposite -- ortho(0, w, h, 0) -- because its
        // glyph rects are laid out with +y up (see DESIGN.md 26.4 / 27.5).
        cameraBlock.projection = glm::ortho(0.0f, gameWidth, 0.0f, gameHeight, -1.0f, 1.0f);

        for (size_t kind = 0; kind < spriteKinds.size(); ++kind) {
            SpriteKind &target = spriteKinds[kind];
            if (target.instances.size() > target.capacity) {
                throw std::runtime_error("breakout: " + std::string(kSpriteKinds[kind].name)
                                         + " instance bucket overflow");
            }
            target.draw->OnUpdateRawData(ResourceId(0, 0), frame, cameraBlock);
            target.pipeline->setInstanceCount(static_cast<uint32_t>(target.instances.size()));
            if (!target.instances.empty()) {
                target.draw->OnUpdateRawData(ResourceId(1, 0), frame, target.instances);
            }
        }

        particleDraw->OnUpdateRawData(ResourceId(0, 0), frame, cameraBlock);
        particlePipeline->setInstanceCount(static_cast<uint32_t>(particleInstances.size()));
        if (!particleInstances.empty()) {
            particleDraw->OnUpdateRawData(ResourceId(1, 0), frame, particleInstances);
        }

        PostBlock postBlock;
        const bool forceShake = options.effect == ForcedEffect::eShake;
        const bool forceConfuse = options.effect == ForcedEffect::eConfuse;
        const bool forceChaos = options.effect == ForcedEffect::eChaos;
        postBlock.params = glm::vec4(simTime, (game.confuse || forceConfuse) ? 1.0f : 0.0f,
                                     (game.chaos || forceChaos) ? 1.0f : 0.0f,
                                     (game.shake || forceShake) ? 1.0f : 0.0f);
        postDraw->OnUpdateRawData(ResourceId(0, 0), frame, postBlock);

        textDraw->OnUpdateRawData(ResourceId(0, 0), frame, cameraBlock);
        textPipeline->setInstanceCount(static_cast<uint32_t>(glyphQuads.size()));
        if (!glyphQuads.empty()) {
            textDraw->OnUpdateRawData(ResourceId(2, 0), frame, glyphQuads);
        }

        // --- one-off data path check -----------------------------------------
        // Frame 0 only records the snapshot; the comparison runs once the first
        // frame has actually been submitted, otherwise the device copy has not
        // happened yet and the buffer still holds its allocation contents.
        const SpriteKind &block = spriteKinds[static_cast<size_t>(Sprite::eBlock)];
        if (options.verify && !verified && !verifyPending) {
            verifySnapshot = block.instances;
            verifyPending = true;
        } else if (verifyPending && !verified) {
            const size_t expected = verifySnapshot.size() * sizeof(SpriteInstance);
            auto probe = mgr->UploadSequence();
            const auto bytes = probe->ReadBack(block.asset, ResourceId(1, 0), expected);
            const bool same = bytes.size() >= expected
                              && std::memcmp(bytes.data(), verifySnapshot.data(), expected) == 0;
            std::cout << "[verify] " << kSpriteKinds[static_cast<size_t>(Sprite::eBlock)].name << " instances: "
                      << verifySnapshot.size() << " x " << sizeof(SpriteInstance) << " B, equals CPU array: "
                      << (same ? "yes" : "NO") << "\n";
            if (!same) {
                throw std::runtime_error("breakout: the sprite instance buffer does not match the CPU array");
            }
            verified = true;
        }

        // --- record ----------------------------------------------------------
        sequence->beginRecording();
        // Every asset whose staging buffer changed this frame copies it into
        // the device buffer here; a missing one silently draws last frame's
        // data (the 24.12 pitfall).
        for (const SpriteKind &target : spriteKinds) {
            sequence->record(target.sync);
        }
        sequence->record(particleSync);
        sequence->record(postFrameSync);
        sequence->record(textFrameSync);

        // Scene: everything up to (but excluding) the ball, then the particles,
        // then the ball -- the reference's draw order.
        sequence->recordPass(scenePass, [&]() {
            for (uint32_t kind = 0; kind < static_cast<uint32_t>(Sprite::eBall); ++kind) {
                if (spriteKinds[kind].instances.empty()) {
                    continue;
                }
                sequence->record(spriteKinds[kind].draw);
            }
            if (!particleInstances.empty()) {
                sequence->record(particleDraw);
            }
            if (!spriteKinds[static_cast<size_t>(Sprite::eBall)].instances.empty()) {
                sequence->record(spriteKinds[static_cast<size_t>(Sprite::eBall)].draw);
            }
        });

        // Screen: post-processing, then the HUD on top of it (the reference
        // renders text after the post-process quad for exactly this reason).
        sequence->recordPass(screenPass, [&]() {
            sequence->record(postDraw);
            if (!glyphQuads.empty()) {
                sequence->record(textDraw);
            }
        });

        sequence->endRecording();
        sequence->submitFrame();

        ++framesRendered;
        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            auto readback = ImageReadback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                          mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                          *mgr->getQueueFamilyIndices().graphicsFamily);
            DumpRequest frameRequest;
            frameRequest.label = options.effect == ForcedEffect::eNone ? "frame" : "frame-effect";
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = swapFormat;
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;

            // The scene target is linear UNORM while the frame is sRGB-encoded
            // by the swapchain, so the two means differ by the transfer curve
            // (~0.735 vs 0.5 for mid grey).  The scene dump is there to separate
            // "the post-processor did this" from "the scene really looks like
            // that".
            DumpRequest sceneRequest;
            sceneRequest.label = "scene";
            sceneRequest.image = scenePass->offscreenImages()[frame];
            sceneRequest.format = sceneFormat;
            sceneRequest.extent = scenePass->extent();
            sceneRequest.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            sceneRequest.encoding = DumpEncoding::eColor;

            const auto results = DumpImages(readback, {frameRequest, sceneRequest}, "dump", 0);
            for (const DumpResult &result : results) {
                std::cout << "[dump] " << result.path.string() << " mean=(" << result.meanColor.r << ","
                          << result.meanColor.g << "," << result.meanColor.b << ")\n";
            }
            dumpDone = true;
            if (!sequence->presentFrame()) {
                continue;
            }
            break;
        }

        if (!sequence->presentFrame()) {
            continue;
        }
        if (options.frames != 0 && framesRendered >= options.frames) {
            break;
        }
    }

    if (options.verify && !verified) {
        throw std::runtime_error("breakout: --verify needs at least two frames to read the buffer back");
    }

    if (options.stats) {
        std::cout << "[stats] frames=" << framesRendered << " level=" << game.level << " lives=" << game.lives
                  << " bricksDestroyed=" << game.bricksDestroyed << " bricksAlive="
                  << game.levels[game.level].aliveCount() << " powerUpsSpawned=" << game.powerUpsSpawned
                  << " powerUpsCollected=" << game.powerUpsCollected << " livesLost=" << game.livesLost
                  << " levelsCleared=" << game.levelsCleared << "\n";
        std::cout << "[stats] ball=(" << game.ball.position.x << "," << game.ball.position.y << ") paddle="
                  << game.player.position.x << " particlesAlive=" << game.particles.aliveCount()
                  << " effect=" << (game.chaos ? "chaos" : game.confuse ? "confuse" : game.shake ? "shake" : "none")
                  << "\n";
    }
}

int main(int argc, char **argv)
{
    try {
        test(argc, argv);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
