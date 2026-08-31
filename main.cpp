#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <SFML/Window.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <thread>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "Expression.hpp"

namespace cfg {
    constexpr unsigned W = 1280, H = 720;
    constexpr unsigned short PORT = 42069;
}

enum class Screen { Menu, Settings, HostWait, Connect, Game };
enum class Msg : std::int32_t { Settings=2, Start=3, Shoot=4, State=5, Disconnect=7 };

struct Settings {
    float scale = 1.0f;
    float turnSeconds = 60.0f;
    int fieldWidth = 2400;
    int fieldHeight = 1200;
    bool prediction = true;
};

struct Player {
    float x = 0, y = 0;
    int hp = 100;
};

struct Obstacle {
    float x = 0, y = 0, r = 30;
    int hp = 1;
    int maxHp = 1;
};

struct HistoryEntry {
    std::string expression;
    std::array<double, 3> coefficients{1.0, 0.0, 0.0};
};

struct Shot {
    bool active = false;
    float t = 0;
    float startX = 0, startY = 0;
    float speed = 1.0f;
    float c = 0;
    int dir = 1;
    int owner = 0;
    std::string expr;
};

static std::string fmt(float v, int p = 2) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(p) << v;
    return s.str();
}

class Net {
public:
    bool host = false;
    bool connected = false;
    sf::TcpListener listener;
    sf::TcpSocket socket;

    bool startHost() {
        listener.close();
        connected = false;
        host = false;

        for (int attempt = 0; attempt < 8; ++attempt) {
            if (listener.listen(cfg::PORT) == sf::Socket::Status::Done) {
                listener.setBlocking(false);
                host = true;
                return true;
            }
            listener.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        return false;
    }

    bool acceptClient() {
        if (connected) return true;
        auto c = std::make_unique<sf::TcpSocket>();
        if (listener.accept(*c) == sf::Socket::Status::Done) {
            socket = std::move(*c);
            socket.setBlocking(false);
            connected = true;
            listener.close();
            return true;
        }
        return false;
    }

    bool connectTo(const std::string& ip) {
        auto address = sf::IpAddress::fromString(ip);
        if (!address) return false;

        // A socket used by the previous match may still be in a closing state.
        // Reuse the object only after disconnecting it and retry the connection.
        socket.disconnect();
        connected = false;
        host = false;

        for (int attempt = 0; attempt < 6; ++attempt) {
            socket.setBlocking(true);
            if (socket.connect(*address, cfg::PORT, sf::seconds(1.0f)) == sf::Socket::Status::Done) {
                socket.setBlocking(false);
                connected = true;
                return true;
            }
            socket.disconnect();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        return false;
    }

    void send(sf::Packet& packet) {
        if (connected) (void)socket.send(packet);
    }

    bool receive(sf::Packet& packet) {
        if (!connected) return false;
        return socket.receive(packet) == sf::Socket::Status::Done;
    }

    void close() {
        socket.disconnect();
        listener.close();
        connected = false;
        host = false;
    }
};

class GameApp {
    sf::RenderWindow window{sf::VideoMode({cfg::W, cfg::H}), "GraphWar Clone"};
    sf::Font font;
    sf::View worldView;

    Screen screen = Screen::Menu;
    Settings settings;
    Net net;

    Player players[2];
    std::vector<Obstacle> obstacles;
    Shot shot;

    int localPlayer = 0;
    int turn = 0;
    float turnLeft = 60.0f;
    float power = 1.4f;
    float constant = 0.0f;
    std::array<double, 3> coefficients{1.0, 0.0, 0.0};

    std::string function = "sin(x)";
    std::string input;
    std::string error;

    Expression compiled;

    std::vector<HistoryEntry> functionHistory{{"sin(x)", {1.0, 0.0, 0.0}}};
    int historyCursor = -1;

    std::mt19937 rng{std::random_device{}()};
    sf::Clock clock;
    float syncTimer = 0.0f;
    bool matchStarted = false;
    bool editingFunction = false;
    bool fullscreen = false;
    int selectedSetting = 0;

public:
    GameApp() {
        window.setFramerateLimit(120);
        (void)font.openFromFile("assets/arial.ttf");
        window.setKeyRepeatEnabled(true);
        compileFunction();
        updateWorldView();
    }

    void run() {
        while (window.isOpen()) {
            float dt = clock.restart().asSeconds();
            events();
            network();
            update(std::min(dt, 0.05f));
            render();
        }
    }

private:
    int shotDirection(int shooter) const {
        const int enemy = 1 - shooter;
        return players[enemy].x >= players[shooter].x ? 1 : -1;
    }

    void randomizePlayers() {
        const float marginX = std::max(100.0f, settings.fieldWidth * 0.06f);
        const float minY = settings.fieldHeight * 0.25f;
        const float maxY = settings.fieldHeight * 0.75f;

        std::uniform_real_distribution<float> xDist(marginX, settings.fieldWidth - marginX);
        std::uniform_real_distribution<float> yDist(minY, maxY);

        float x0 = xDist(rng);
        float x1 = xDist(rng);
        for (int i = 0; i < 200 && std::abs(x1 - x0) < settings.fieldWidth * 0.32f; ++i)
            x1 = xDist(rng);

        players[0] = {x0, yDist(rng), 100};
        players[1] = {x1, yDist(rng), 100};
    }

    void generateObstacles() {
        obstacles.clear();

        const float left = std::min(players[0].x, players[1].x);
        const float right = std::max(players[0].x, players[1].x);
        const float span = right - left;

        std::uniform_int_distribution<int> countDist(8, 16);
        std::uniform_real_distribution<float> radiusDist(24.0f, 75.0f);
        std::uniform_real_distribution<float> yDist(
            settings.fieldHeight * 0.12f,
            settings.fieldHeight * 0.88f);

        int target = countDist(rng);
        for (int attempt = 0; attempt < 500 && static_cast<int>(obstacles.size()) < target; ++attempt) {
            float minX = left + std::min(140.0f, span * 0.12f);
            float maxX = right - std::min(140.0f, span * 0.12f);
            if (maxX <= minX) break;

            std::uniform_real_distribution<float> xDist(minX, maxX);
            const int hp = 1 + static_cast<int>(rng() % 3);
            Obstacle o{xDist(rng), yDist(rng), radiusDist(rng), hp, hp};

            bool valid = true;
            for (const auto& p : players) {
                if (std::hypot(o.x - p.x, o.y - p.y) < o.r + 100.0f) {
                    valid = false;
                    break;
                }
            }
            if (!valid) continue;

            for (const auto& other : obstacles) {
                if (std::hypot(o.x - other.x, o.y - other.y) < o.r + other.r + 35.0f) {
                    valid = false;
                    break;
                }
            }
            if (valid) obstacles.push_back(o);
        }
    }

    void resetPlayers() {
        randomizePlayers();
        generateObstacles();
        shot = {};
        turn = 0;
        turnLeft = settings.turnSeconds;
        constant = 0.0f;
        coefficients = {1.0, 0.0, 0.0};
        power = 1.4f;
    }

    void startMatch() {
        resetPlayers();
        matchStarted = true;
        screen = Screen::Game;
        turnLeft = settings.turnSeconds;
        editingFunction = false;
        historyCursor = -1;
        if (!compileFunction()) {
            function = "sin(x)";
            compileFunction();
        }
    }

    float pixelsPerUnit() const {
        return 85.0f * settings.scale;
    }

    void updateWorldView() {
        const float ww = static_cast<float>(window.getSize().x);
        const float wh = static_cast<float>(window.getSize().y);
        const float fw = static_cast<float>(settings.fieldWidth);
        const float fh = static_cast<float>(settings.fieldHeight);
        const float s = std::min(ww / fw, wh / fh);
        const float vw = fw * s / ww;
        const float vh = fh * s / wh;

        worldView.setCenter({fw / 2.0f, fh / 2.0f});
        worldView.setSize({fw, fh});
        worldView.setViewport(sf::FloatRect(
            {(1.0f - vw) / 2.0f, (1.0f - vh) / 2.0f},
            {vw, vh}));
    }

    void toggleFullscreen() {
        fullscreen = !fullscreen;
        if (fullscreen)
            window.create(sf::VideoMode::getDesktopMode(), "GraphWar Clone", sf::State::Fullscreen);
        else
            window.create(sf::VideoMode({cfg::W, cfg::H}), "GraphWar Clone", sf::Style::Default);

        window.setFramerateLimit(120);
        updateWorldView();
    }

    bool compileFunction() {
        std::string e;
        if (!compiled.parse(function, e)) {
            error = e;
            return false;
        }
        error.clear();
        return true;
    }

    void addFunctionToHistory(const std::string& f) {
        if (f.empty()) return;
        auto it = std::find_if(functionHistory.begin(), functionHistory.end(), [&](const HistoryEntry& h) {
            return h.expression == f && h.coefficients == coefficients;
        });
        if (it != functionHistory.end()) functionHistory.erase(it);
        functionHistory.insert(functionHistory.begin(), {f, coefficients});
        if (functionHistory.size() > 12) functionHistory.resize(12);
        historyCursor = -1;
    }

    void browseHistory(int direction) {
        if (functionHistory.empty()) return;
        if (historyCursor < 0) historyCursor = 0;
        else historyCursor = std::clamp(historyCursor + direction, 0, static_cast<int>(functionHistory.size()) - 1);
        input = functionHistory[historyCursor].expression;
        coefficients = functionHistory[historyCursor].coefficients;
    }

    void events() {
        while (auto ev = window.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) window.close();
            if (ev->is<sf::Event::Resized>()) updateWorldView();
            if (auto* k = ev->getIf<sf::Event::KeyPressed>()) key(k->code);
            if (auto* t = ev->getIf<sf::Event::TextEntered>()) text(t->unicode);
        }
    }

    void text(char32_t u) {
        if (screen == Screen::Settings || screen == Screen::Connect ||
            (screen == Screen::Game && editingFunction)) {
            if (u >= 32 && u < 127 && input.size() < 100)
                input.push_back(static_cast<char>(u));
        }
    }

    void leaveSession() {
        if (net.connected) {
            sf::Packet p;
            p << static_cast<std::int32_t>(Msg::Disconnect);
            net.send(p);
        }
        net.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        screen = Screen::Menu;
        matchStarted = false;
        editingFunction = false;
        historyCursor = -1;
        error.clear();
    }

    void key(sf::Keyboard::Key k) {
        if (k == sf::Keyboard::Key::F11) {
            toggleFullscreen();
            return;
        }

        if (k == sf::Keyboard::Key::Escape && screen != Screen::Menu) {
            leaveSession();
            return;
        }

        if (screen == Screen::Menu) {
            if (k == sf::Keyboard::Key::Num1) {
                net.close();
                net.host = true;
                screen = Screen::Settings;
            } else if (k == sf::Keyboard::Key::Num2) {
                input = "127.0.0.1";
                screen = Screen::Connect;
            }
        } else if (screen == Screen::Settings) {
            settingsKey(k);
        } else if (screen == Screen::Connect) {
            if (k == sf::Keyboard::Key::Enter) {
                if (net.connectTo(input)) {
                    localPlayer = 1;
                    screen = Screen::HostWait;
                } else error = "Connection failed";
            } else if (k == sf::Keyboard::Key::Backspace && !input.empty()) {
                input.pop_back();
            }
        } else if (screen == Screen::Game) {
            gameKey(k);
        }
    }

    void settingsKey(sf::Keyboard::Key k) {
        if (k == sf::Keyboard::Key::Tab) {
            selectedSetting = (selectedSetting + 1) % 5;
            return;
        }

        if (k == sf::Keyboard::Key::Left || k == sf::Keyboard::Key::Right) {
            const int d = k == sf::Keyboard::Key::Right ? 1 : -1;
            if (selectedSetting == 0) settings.fieldWidth = std::clamp(settings.fieldWidth + d * 100, 800, 8000);
            if (selectedSetting == 1) settings.fieldHeight = std::clamp(settings.fieldHeight + d * 100, 600, 4000);
            if (selectedSetting == 2) settings.scale = std::clamp(settings.scale + d * 0.25f, 0.5f, 3.0f);
            if (selectedSetting == 3) settings.turnSeconds = std::clamp(settings.turnSeconds + d * 5.0f, 5.0f, 300.0f);
            if (selectedSetting == 4) settings.prediction = !settings.prediction;
        }

        if (k == sf::Keyboard::Key::Enter) {
            if (net.host) {
                if (net.startHost()) screen = Screen::HostWait;
                else error = "Port 42069 is unavailable";
            }
        }
    }

    void gameKey(sf::Keyboard::Key k) {
        if (editingFunction) {
            if (k == sf::Keyboard::Key::Up) {
                browseHistory(+1);
                return;
            }
            if (k == sf::Keyboard::Key::Down) {
                browseHistory(-1);
                return;
            }
            if (k == sf::Keyboard::Key::Enter) {
                if (!input.empty()) {
                    std::string old = function;
                    function = input;
                    if (compileFunction()) addFunctionToHistory(function);
                    else function = old;
                }
                editingFunction = false;
                historyCursor = -1;
                return;
            }
            if (k == sf::Keyboard::Key::Backspace && !input.empty()) {
                input.pop_back();
                return;
            }
            return;
        }

        if (k == sf::Keyboard::Key::Tab && !shot.active && matchStarted && localPlayer == turn) {
            editingFunction = true;
            input = function;
            historyCursor = -1;
            return;
        }

        if (localPlayer != turn || shot.active || !matchStarted) return;

        if (k == sf::Keyboard::Key::Up) power = std::min(5.0f, power + 0.1f);
        if (k == sf::Keyboard::Key::Down) power = std::max(0.1f, power - 0.1f);
        if (k == sf::Keyboard::Key::Left) constant -= 0.1f;
        if (k == sf::Keyboard::Key::Right) constant += 0.1f;

        // Coefficients: Q/A = a, W/S = b, E/D = c.
        if (k == sf::Keyboard::Key::Q) coefficients[0] += 0.1;
        if (k == sf::Keyboard::Key::A) coefficients[0] -= 0.1;
        if (k == sf::Keyboard::Key::W) coefficients[1] += 0.1;
        if (k == sf::Keyboard::Key::S) coefficients[1] -= 0.1;
        if (k == sf::Keyboard::Key::E) coefficients[2] += 0.1;
        if (k == sf::Keyboard::Key::D) coefficients[2] -= 0.1;

        if (k == sf::Keyboard::Key::F1) { function = "sin(x)"; compileFunction(); addFunctionToHistory(function); }
        if (k == sf::Keyboard::Key::F2) { function = "0.25*x^2"; compileFunction(); addFunctionToHistory(function); }
        if (k == sf::Keyboard::Key::F3) { function = "2*sin(x)"; compileFunction(); addFunctionToHistory(function); }
        if (k == sf::Keyboard::Key::F4) { function = "x/2"; compileFunction(); addFunctionToHistory(function); }

        if (k == sf::Keyboard::Key::F5) browseHistory(1);
        if (k == sf::Keyboard::Key::F6) browseHistory(-1);

        if (k == sf::Keyboard::Key::Space) requestShoot();
    }

    void createShot(int shooter, const std::string& expr, float pw, float c) {
        function = expr;
        power = pw;
        constant = c;
        compileFunction();

        shot = {};
        shot.active = true;
        shot.owner = shooter;
        shot.t = 0.0f;
        shot.startX = players[shooter].x;
        shot.startY = players[shooter].y;
        shot.speed = pw;
        shot.c = c;
        shot.dir = shotDirection(shooter);
        shot.expr = expr;
    }

    void requestShoot() {
        if (!compileFunction()) return;
        addFunctionToHistory(function);

        if (net.host) {
            createShot(localPlayer, function, power, constant);
            broadcastState();
        } else {
            // Client only sends a request. The host creates the authoritative shot.
            sf::Packet p;
            p << static_cast<std::int32_t>(Msg::Shoot) << function << power << constant << coefficients[0] << coefficients[1] << coefficients[2];
            net.send(p);
        }
    }

    void network() {
        if (net.host && !net.connected && screen == Screen::HostWait) {
            if (net.acceptClient()) {
                sf::Packet s;
                s << static_cast<std::int32_t>(Msg::Settings)
                  << settings.scale << settings.turnSeconds
                  << settings.fieldWidth << settings.fieldHeight
                  << static_cast<std::int32_t>(settings.prediction);
                net.send(s);

                s.clear();
                s << static_cast<std::int32_t>(Msg::Start);
                net.send(s);

                startMatch();
                broadcastState();
            }
        }

        if (!net.connected) return;

        sf::Packet p;
        while (net.receive(p)) {
            std::int32_t type;
            if (!(p >> type)) continue;

            if (type == static_cast<std::int32_t>(Msg::Settings)) {
                std::int32_t pred = 1;
                p >> settings.scale >> settings.turnSeconds >> settings.fieldWidth >> settings.fieldHeight >> pred;
                settings.prediction = pred != 0;
                updateWorldView();
            }
            else if (type == static_cast<std::int32_t>(Msg::Start)) {
                localPlayer = 1;
                startMatch();
            }
            else if (type == static_cast<std::int32_t>(Msg::Shoot)) {
                std::string f;
                float pw, c;
                double ca, cb, cc;
                p >> f >> pw >> c >> ca >> cb >> cc;

                if (net.host) {
                    if (turn == 1 && !shot.active) {
                        coefficients = {ca, cb, cc};
                        createShot(1, f, pw, c);
                        broadcastState();
                    }
                }
            }
            else if (type == static_cast<std::int32_t>(Msg::State)) {
                receiveState(p);
            }
            else if (type == static_cast<std::int32_t>(Msg::Disconnect)) {
                net.close();
                screen = Screen::Menu;
                matchStarted = false;
            }
        }
    }

    void receiveState(sf::Packet& p) {
        std::int32_t tr;
        p >> tr >> turnLeft;
        turn = tr;

        for (auto& pl : players) p >> pl.x >> pl.y >> pl.hp;

        std::int32_t active;
        p >> active;
        shot.active = active != 0;
        if (shot.active) {
            p >> shot.t >> shot.startX >> shot.startY >> shot.speed >> shot.c
              >> shot.dir >> shot.owner >> shot.expr;
            p >> coefficients[0] >> coefficients[1] >> coefficients[2];
            function = shot.expr;
            compileFunction();
        }

        std::int32_t count = 0;
        p >> count;
        if (count >= 0 && count <= 100) {
            obstacles.clear();
            obstacles.reserve(count);
            for (std::int32_t i = 0; i < count; ++i) {
                Obstacle o;
                p >> o.x >> o.y >> o.r >> o.hp >> o.maxHp;
                obstacles.push_back(o);
            }
        }

        std::int32_t winner;
        p >> winner;
        if (winner >= 0) matchStarted = false;
    }

    void broadcastState(int winner = -1) {
        sf::Packet p;
        p << static_cast<std::int32_t>(Msg::State)
          << static_cast<std::int32_t>(turn) << turnLeft;

        for (const auto& pl : players) p << pl.x << pl.y << pl.hp;

        p << static_cast<std::int32_t>(shot.active);
        if (shot.active) {
            p << shot.t << shot.startX << shot.startY << shot.speed << shot.c
              << shot.dir << shot.owner << shot.expr;
            p << coefficients[0] << coefficients[1] << coefficients[2];
        }

        p << static_cast<std::int32_t>(obstacles.size());
        for (const auto& o : obstacles) p << o.x << o.y << o.r << o.hp << o.maxHp;

        p << static_cast<std::int32_t>(winner);
        net.send(p);
    }

    void endTurn() {
        shot.active = false;
        turn = 1 - turn;
        turnLeft = settings.turnSeconds;
        compileFunction();
        if (net.host) broadcastState();
    }

    void update(float dt) {
        if (screen != Screen::Game || !matchStarted) return;

        if (net.host || !net.connected) {
            turnLeft -= dt;
            if (turnLeft <= 0.0f && !shot.active) endTurn();
            if (shot.active) updateShot(dt);

            if (net.host) {
                syncTimer += dt;
                if (syncTimer > 0.05f) {
                    syncTimer = 0.0f;
                    broadcastState();
                }
            }
        }
    }

    void hitObstacle(std::size_t index) {
        auto& o = obstacles[index];
        --o.hp;
        // Radius never changes. A ball is damaged until its HP reaches zero,
        // then the whole obstacle is destroyed.
        if (o.hp <= 0)
            obstacles.erase(obstacles.begin() + static_cast<std::ptrdiff_t>(index));

        shot.active = false;
        endTurn();
    }

    void updateShot(float dt) {
        shot.t += dt;

        const float px = shot.startX + shot.dir * shot.t * shot.speed * 260.0f;
        const double gx = (px - shot.startX) / pixelsPerUnit();
        const double gy = compiled.eval(gx, coefficients) + shot.c;
        const float py = shot.startY - static_cast<float>(gy * pixelsPerUnit());

        if (!std::isfinite(py) || px < -100.0f || px > settings.fieldWidth + 100.0f ||
            py < -100.0f || py > settings.fieldHeight + 100.0f) {
            endTurn();
            return;
        }

        const int enemy = 1 - shot.owner;
        if (std::hypot(px - players[enemy].x, py - players[enemy].y) < 30.0f) {
            players[enemy].hp = std::max(0, players[enemy].hp - 25);
            shot.active = false;

            if (players[enemy].hp <= 0) {
                matchStarted = false;
                if (net.host) broadcastState(shot.owner);
                return;
            }

            endTurn();
            return;
        }

        for (std::size_t i = 0; i < obstacles.size(); ++i) {
            const auto& o = obstacles[i];
            if (std::hypot(px - o.x, py - o.y) <= o.r + 7.0f) {
                hitObstacle(i);
                return;
            }
        }
    }

    void render() {
        window.clear(sf::Color(15, 18, 25));

        if (screen == Screen::Game) {
            window.setView(worldView);
            drawGrid();
            drawObstacles();
            drawPlayers();
            drawTrajectory();
            window.setView(window.getDefaultView());
            drawGameUI();
        } else {
            window.setView(window.getDefaultView());
            if (screen == Screen::Menu) drawMenu();
            else if (screen == Screen::Settings) drawSettings();
            else if (screen == Screen::Connect) drawConnect();
            else if (screen == Screen::HostWait) drawWait();
        }

        window.display();
    }

    void txt(const std::string& s, float x, float y, unsigned size = 24) {
        if (font.getInfo().family.empty()) return;
        sf::Text t(font, s, size);
        t.setPosition({x, y});
        t.setFillColor(sf::Color::White);
        window.draw(t);
    }

    void drawMenu() {
        txt("GRAPHWAR CLONE", 430, 100, 42);
        txt("1  Host LAN game", 450, 250, 28);
        txt("2  Join LAN game", 450, 300, 28);
        txt("F11  fullscreen", 450, 350, 22);
        txt("ESC  Exit", 450, 385, 22);
    }

    void drawSettings() {
        txt("MATCH SETTINGS", 460, 55, 38);
        txt(std::string(selectedSetting == 0 ? "> " : "  ") + "Field width: " + std::to_string(settings.fieldWidth), 300, 150, 26);
        txt(std::string(selectedSetting == 1 ? "> " : "  ") + "Field height: " + std::to_string(settings.fieldHeight), 300, 195, 26);
        txt(std::string(selectedSetting == 2 ? "> " : "  ") + "Graph scale: " + fmt(settings.scale) + "x", 300, 240, 26);
        txt(std::string(selectedSetting == 3 ? "> " : "  ") + "Turn time: " + fmt(settings.turnSeconds, 0) + " sec", 300, 285, 26);
        txt(std::string(selectedSetting == 4 ? "> " : "  ") + "Trajectory prediction: " + std::string(settings.prediction ? "ON" : "OFF"), 300, 330, 26);
        txt("TAB select   LEFT/RIGHT change", 300, 400, 20);
        txt("ENTER create LAN lobby", 300, 435, 24);
        txt("F11 fullscreen   ESC back", 300, 475, 20);
        if (!error.empty()) txt(error, 300, 530, 20);
    }

    void drawConnect() {
        txt("JOIN LAN GAME", 450, 100, 38);
        txt("Server IP:", 350, 230, 26);
        txt(input, 550, 230, 26);
        txt("ENTER  connect", 350, 300, 24);
        txt("ESC  back", 350, 340, 22);
        if (!error.empty()) txt(error, 350, 430, 20);
    }

    void drawWait() {
        txt("WAITING FOR SECOND PLAYER", 360, 150, 36);
        txt("TCP port: 42069", 480, 220, 24);
        txt("Field: " + std::to_string(settings.fieldWidth) + " x " + std::to_string(settings.fieldHeight), 480, 265, 22);
        txt("Scale: " + fmt(settings.scale) + "x", 480, 300, 22);
        txt("Turn time: " + fmt(settings.turnSeconds, 0) + " sec", 480, 335, 22);
        txt("Prediction: " + std::string(settings.prediction ? "ON" : "OFF"), 480, 370, 22);
        txt("Tell the other player your LAN IPv4 address.", 320, 430, 22);
        txt("ESC  cancel", 480, 500, 20);
    }

    void drawGameUI() {
        txt("P" + std::to_string(turn + 1) + " TURN   " + fmt(turnLeft, 1) + "s", 20, 15, 24);
        txt("f(x): " + function, 20, 50, 20);
        txt("C: " + fmt(constant) + "   a: " + fmt(static_cast<float>(coefficients[0])) + "   b: " + fmt(static_cast<float>(coefficients[1])) + "   c: " + fmt(static_cast<float>(coefficients[2])), 20, 78, 20);
        txt("TAB edit   LEFT/RIGHT C   UP/DOWN power   Q/A a   W/S b   E/D c   SPACE fire   F11 fullscreen", 20, 108, 16);
        txt(std::string("Prediction: ") + (settings.prediction ? "ON" : "OFF") +
            "   Field: " + std::to_string(settings.fieldWidth) + " x " + std::to_string(settings.fieldHeight), 20, 132, 17);

        if (editingFunction)
            txt("EDIT: " + input + "   ENTER apply   UP/DOWN history   BACKSPACE delete", 20, 158, 19);

        const float historyX = static_cast<float>(window.getSize().x) - 330.0f;
        txt("FUNCTION HISTORY", historyX, 20, 19);
        for (std::size_t i = 0; i < std::min<std::size_t>(functionHistory.size(), 8); ++i) {
            txt(std::to_string(i + 1) + ". " + functionHistory[i].expression + " [" + fmt(static_cast<float>(functionHistory[i].coefficients[0])) + "," + fmt(static_cast<float>(functionHistory[i].coefficients[1])) + "," + fmt(static_cast<float>(functionHistory[i].coefficients[2])) + "]", historyX, 48.0f + i * 25.0f, 14);
        }

        txt("P1 HP " + std::to_string(players[0].hp), 40, window.getSize().y - 35.0f, 20);
        txt("P2 HP " + std::to_string(players[1].hp), window.getSize().x - 170.0f, window.getSize().y - 35.0f, 20);

        if (!matchStarted)
            txt(players[0].hp <= 0 ? "PLAYER 2 WINS" : "PLAYER 1 WINS",
                window.getSize().x / 2.0f - 150.0f, window.getSize().y / 2.0f, 42);
    }

    static sf::Vertex makeVertex(sf::Vector2f position, sf::Color color) {
        sf::Vertex v;
        v.position = position;
        v.color = color;
        return v;
    }

    void drawGrid() {
        sf::VertexArray lines(sf::PrimitiveType::Lines);
        for (int x = 0; x <= settings.fieldWidth; x += 100) {
            lines.append(makeVertex({static_cast<float>(x), 0.0f}, sf::Color(32, 36, 47)));
            lines.append(makeVertex({static_cast<float>(x), static_cast<float>(settings.fieldHeight)}, sf::Color(32, 36, 47)));
        }
        for (int y = 0; y <= settings.fieldHeight; y += 100) {
            lines.append(makeVertex({0.0f, static_cast<float>(y)}, sf::Color(32, 36, 47)));
            lines.append(makeVertex({static_cast<float>(settings.fieldWidth), static_cast<float>(y)}, sf::Color(32, 36, 47)));
        }
        window.draw(lines);
    }

    void drawObstacles() {
        for (const auto& o : obstacles) {
            sf::CircleShape c(o.r);
            c.setOrigin({o.r, o.r});
            c.setPosition({o.x, o.y});
            c.setFillColor(sf::Color(58, 62, 76));
            c.setOutlineThickness(3.0f);
            c.setOutlineColor(o.hp == o.maxHp ? sf::Color(135, 140, 155) : sf::Color(190, 150, 90));
            window.draw(c);
            if (o.hp < o.maxHp) {
                sf::CircleShape damage(o.r * 0.42f);
                damage.setOrigin({o.r * 0.42f, o.r * 0.42f});
                damage.setPosition({o.x - o.r * 0.18f, o.y + o.r * 0.12f});
                damage.setFillColor(sf::Color(0, 0, 0, 0));
                damage.setOutlineThickness(2.0f);
                damage.setOutlineColor(sf::Color(180, 100, 80));
                window.draw(damage);
            }
        }
    }

    void drawPlayers() {
        for (int i = 0; i < 2; ++i) {
            sf::CircleShape c(22);
            c.setOrigin({22, 22});
            c.setPosition({players[i].x, players[i].y});
            c.setFillColor(i == 0 ? sf::Color(80, 160, 255) : sf::Color(255, 90, 90));
            window.draw(c);
        }
    }

    void drawTrajectory() {
        if (shot.active) {
            std::vector<sf::Vertex> pts;
            for (int i = 0; i < 70; ++i) {
                float tt = shot.t + i * 0.035f;
                float px = shot.startX + shot.dir * tt * shot.speed * 260.0f;
                double gx = (px - shot.startX) / pixelsPerUnit();
                double gy = compiled.eval(gx, coefficients) + shot.c;
                float py = shot.startY - static_cast<float>(gy * pixelsPerUnit());
                if (std::isfinite(py) && px >= 0 && px <= settings.fieldWidth && py >= 0 && py <= settings.fieldHeight)
                    pts.emplace_back(makeVertex({px, py}, sf::Color(230, 230, 230)));
            }
            if (pts.size() > 1) window.draw(pts.data(), pts.size(), sf::PrimitiveType::LineStrip);

            float px = shot.startX + shot.dir * shot.t * shot.speed * 260.0f;
            double gx = (px - shot.startX) / pixelsPerUnit();
            double gy = compiled.eval(gx, coefficients) + shot.c;
            float py = shot.startY - static_cast<float>(gy * pixelsPerUnit());
            sf::CircleShape b(7);
            b.setOrigin({7, 7});
            b.setPosition({px, py});
            b.setFillColor(sf::Color::White);
            window.draw(b);
        }
        else if (settings.prediction && localPlayer == turn) {
            std::vector<sf::Vertex> pts;
            const int dir = shotDirection(turn);
            for (int i = 0; i < 55; ++i) {
                float px = players[turn].x + dir * i * 4.0f;
                double gx = (px - players[turn].x) / pixelsPerUnit();
                double gy = compiled.eval(gx, coefficients) + constant;
                float py = players[turn].y - static_cast<float>(gy * pixelsPerUnit());
                if (std::isfinite(py) && px >= 0 && px <= settings.fieldWidth && py >= 0 && py <= settings.fieldHeight)
                    pts.emplace_back(makeVertex({px, py}, sf::Color(120, 180, 255)));
            }
            if (pts.size() > 1) window.draw(pts.data(), pts.size(), sf::PrimitiveType::LineStrip);
        }
    }
};

int main() {
    GameApp app;
    app.run();
}
