#include "pipeduck/tune.hpp"

#include <ncurses.h>
#include <unistd.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <string>

#include "pipeduck/config.hpp"
#include "pipeduck/duck.hpp"
#include "pipeduck/vad.hpp"

namespace pipeduck {
namespace {

enum class Row { Trigger, Sensitivity, MicSensitivity, DuckPercent, Hold, AttackFade, ReleaseFade };

constexpr Row kRows[] = {Row::Trigger, Row::Sensitivity, Row::MicSensitivity, Row::DuckPercent,
                         Row::Hold,    Row::AttackFade,  Row::ReleaseFade};

class CursesGuard {
public:
    CursesGuard() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            throw std::runtime_error("the terminal tuner needs an interactive terminal");
        }
        setlocale(LC_ALL, "");
        if (initscr() == nullptr) {
            throw std::runtime_error("failed to initialize the terminal");
        }
        active_ = true;
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        timeout(250);
    }

    ~CursesGuard() {
        if (active_) {
            endwin();
        }
    }

private:
    bool active_ = false;
};

double sensitivity_percent(double threshold) {
    constexpr double min = 0.0025;
    constexpr double max = 0.2;
    const double normalized = 1.0 - ((std::clamp(threshold, min, max) - min) / (max - min));
    return normalized * 100.0;
}

std::string bar(double percent) {
    const int filled = static_cast<int>((std::clamp(percent, 0.0, 100.0) + 5.0) / 10.0);
    return std::string(static_cast<std::size_t>(std::clamp(filled, 0, 10)), '#') +
           std::string(static_cast<std::size_t>(10 - std::clamp(filled, 0, 10)), '-');
}

void save(const DuckingSettings& settings) {
    Config config = Config::load_or_default();
    settings.apply_to(config);
    config.save();
}

void adjust(DuckingSettings& settings, Row row, int direction) {
    switch (row) {
    case Row::Trigger: {
        const int index = (static_cast<int>(settings.trigger) + direction) % 3;
        const int wrapped = (index + 3) % 3;
        settings.trigger = static_cast<Trigger>(wrapped);
        break;
    }
    case Row::Sensitivity:
        settings.vad_threshold -= static_cast<double>(direction) * 0.0025;
        break;
    case Row::MicSensitivity:
        settings.mic_vad_threshold -= static_cast<double>(direction) * 0.0025;
        break;
    case Row::DuckPercent:
        settings.duck_percent += direction;
        break;
    case Row::Hold:
        settings.hold_ms += direction * 50;
        break;
    case Row::AttackFade:
        settings.attack_fade_ms += direction * 10;
        break;
    case Row::ReleaseFade:
        settings.release_fade_ms += direction * 10;
        break;
    }
    settings = settings.normalized();
}

void draw(const DuckingSettings& settings, Row selected) {
    erase();
    mvprintw(0, 0, "pipeduck Tuner");
    mvprintw(2, 0, "Up/Down select - Left/Right adjust - q/Esc close");
    mvprintw(3, 0, "Changes are saved immediately and apply live via config.toml");
    mvprintw(5, 0, "Rows:");

    const int row_count = static_cast<int>(std::size(kRows));
    for (int index = 0; index < row_count; ++index) {
        const Row row = kRows[index];
        std::string label;
        std::string value;
        double percent = 0.0;
        switch (row) {
        case Row::Trigger:
            label = "Trigger";
            value = trigger_name(settings.trigger);
            if (settings.trigger == Trigger::Call)
                value += " (remote call audio)";
            if (settings.trigger == Trigger::Mic)
                value += " (my microphone)";
            if (settings.trigger == Trigger::Both)
                value += " (call or mic)";
            percent = static_cast<double>(static_cast<int>(settings.trigger)) / 2.0 * 100.0;
            break;
        case Row::Sensitivity:
            label = "Sensitivity (call)";
            percent = sensitivity_percent(settings.vad_threshold);
            if (vad_is_disabled(settings.vad_threshold)) {
                value = "  0%  OFF";
            } else {
                char buffer[128];
                std::snprintf(buffer, sizeof(buffer), "%3.0f%%  threshold %.4f, start %.4f",
                              percent, settings.vad_threshold,
                              vad_start_threshold(settings.vad_threshold));
                value = buffer;
            }
            break;
        case Row::MicSensitivity:
            label = "Microphone sens.";
            percent = sensitivity_percent(settings.mic_vad_threshold);
            if (vad_is_disabled(settings.mic_vad_threshold)) {
                value = "  0%  OFF";
            } else {
                char buffer[128];
                std::snprintf(buffer, sizeof(buffer), "%3.0f%%  threshold %.4f, start %.4f",
                              percent, settings.mic_vad_threshold,
                              vad_start_threshold(settings.mic_vad_threshold));
                value = buffer;
            }
            break;
        case Row::DuckPercent:
            label = "Ducking volume";
            percent = settings.duck_percent;
            value = std::to_string(settings.duck_percent) + "%";
            break;
        case Row::Hold:
            label = "Hold";
            percent = settings.hold_ms * 100.0 / 4000.0;
            value = std::to_string(settings.hold_ms) + " ms";
            break;
        case Row::AttackFade:
            label = "Duck fade";
            percent = settings.attack_fade_ms * 100.0 / 2000.0;
            value = std::to_string(settings.attack_fade_ms) + " ms";
            break;
        case Row::ReleaseFade:
            label = "Return fade";
            percent = settings.release_fade_ms * 100.0 / 3000.0;
            value = std::to_string(settings.release_fade_ms) + " ms (smooth return)";
            break;
        }

        if (row == selected) {
            attron(A_REVERSE);
        }
        mvprintw(7 + index, 0, "%-20s [%s] %s", label.c_str(), bar(percent).c_str(), value.c_str());
        if (row == selected) {
            attroff(A_REVERSE);
        }
    }

    mvprintw(15, 0, "Hint: if it ducks without speech, lower sensitivity (raise the threshold).");
    mvprintw(16, 0, "Config: %s", config_path().c_str());
    refresh();
}

} // namespace

int run_terminal_tuner() {
    CursesGuard terminal;
    DuckingSettings settings = DuckingSettings::from_config(Config::load_or_default());
    Row selected = Row::Sensitivity;

    for (;;) {
        draw(settings, selected);
        const int key = getch();
        if (key == ERR) {
            continue;
        }
        switch (key) {
        case 'q':
        case 'Q':
        case 27:
            return 0;
        case KEY_UP:
        case 'k':
            selected = kRows[(static_cast<int>(selected) + static_cast<int>(std::size(kRows)) - 1) %
                             static_cast<int>(std::size(kRows))];
            break;
        case KEY_DOWN:
        case 'j':
            selected = kRows[(static_cast<int>(selected) + 1) % static_cast<int>(std::size(kRows))];
            break;
        case KEY_LEFT:
        case 'h':
        case '-':
            adjust(settings, selected, -1);
            save(settings);
            break;
        case KEY_RIGHT:
        case 'l':
        case '+':
            adjust(settings, selected, 1);
            save(settings);
            break;
        case KEY_RESIZE:
            break;
        default:
            break;
        }
    }
}

} // namespace pipeduck
