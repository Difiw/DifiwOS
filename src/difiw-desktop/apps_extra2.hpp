// Difiw 0.5.1 applications: Notes (to-do list) and Settings (a normal window).
// Included by apps.cpp after apps_extra.hpp, inside its anonymous namespace.
// SPDX-License-Identifier: MIT
#pragma once

// ------------------------------------------------------------------- Notes (to-do list)
struct Todo : App {
    struct Item { bool done = false; std::string text; };
    std::vector<Item> items;
    std::string entry, path;
    int scroll = 0;

    void load() {
        const char* home = std::getenv("HOME");
        path = std::string(home ? home : "/tmp") + "/todo.txt";
        std::ifstream f(path);
        std::string l;
        while (std::getline(f, l)) if (l.size() > 2 && l[1] == '|') items.push_back({l[0] == '1', l.substr(2)});
    }
    void save() {
        std::ofstream f(path, std::ios::trunc);
        for (const auto& i : items) f << (i.done ? "1|" : "0|") << i.text << "\n";
    }
    void on_close() override { save(); quit = true; }
    int row_h() const { return 34 * sc(); }
    Rect input_r() const { return {16 * sc(), 16 * sc(), w - 32 * sc() - 96 * sc(), 36 * sc()}; }
    Rect add_r() const { return {w - 16 * sc() - 88 * sc(), 16 * sc(), 88 * sc(), 36 * sc()}; }
    Rect row_r(int i) const { return {16 * sc(), 68 * sc() + i * row_h(), w - 32 * sc(), row_h() - 4 * sc()}; }
    int rows() const { return std::max(1, (h - 68 * sc() - 40 * sc()) / row_h()); }
    void add() {
        if (entry.empty()) return;
        items.push_back({false, entry});
        entry.clear();
        save();
    }
    void draw(Surface& s) override {
        rrect(s, input_r(), 10 * sc(), kGraphite900);
        rrect_outline(s, input_r(), 10 * sc(), 1, kAccentBlue);
        draw_text(s, input_r().x + 12 * sc(), input_r().y + (input_r().h - font_h() * sc()) / 2,
                  entry.empty() ? t("New task...", "Новая задача...") : fit_text(entry, input_r().w - 24 * sc(), sc()) + "_", entry.empty() ? kTextFaint : kText, sc());
        button(s, add_r(), t("Add", "Добавить"), true);
        int done = 0;
        for (const auto& i : items) done += i.done;
        scroll = clampi(scroll, 0, std::max(0, static_cast<int>(items.size()) - rows()));
        for (int k = 0; k < rows() && scroll + k < static_cast<int>(items.size()); ++k) {
            const Item& it = items[static_cast<std::size_t>(scroll + k)];
            const Rect r = row_r(k);
            if (r.has(mx, my)) rrect(s, r, 8 * sc(), kGraphite700);
            const Rect cb = {r.x + 8 * sc(), r.y + (r.h - 20 * sc()) / 2, 20 * sc(), 20 * sc()};
            rrect(s, cb, 5 * sc(), it.done ? kOk : kGraphite600);
            if (it.done) {
                line(s, cb.x + 4 * sc(), cb.y + 10 * sc(), cb.x + 8 * sc(), cb.y + 15 * sc(), 0xFF0E1118u, 2 * sc());
                line(s, cb.x + 8 * sc(), cb.y + 15 * sc(), cb.x + 16 * sc(), cb.y + 5 * sc(), 0xFF0E1118u, 2 * sc());
            }
            draw_text(s, cb.right() + 12 * sc(), r.y + (r.h - font_h() * sc()) / 2, fit_text(it.text, r.w - 100 * sc(), sc()), it.done ? kTextFaint : kText, sc());
            draw_text(s, r.right() - 24 * sc(), r.y + (r.h - font_h() * sc()) / 2, "x", r.has(mx, my) ? kDanger : kTextFaint, sc());
        }
        if (items.empty()) draw_text(s, 20 * sc(), 76 * sc(), t("No tasks yet. Type one above and press Enter.", "Задач пока нет. Введите задачу и нажмите Enter."), kTextDim, sc());
        char info[64];
        std::snprintf(info, sizeof info, "%d / %d", done, static_cast<int>(items.size()));
        draw_text(s, 16 * sc(), h - 26 * sc(), t("Done: ", "Выполнено: ") + std::string(info), kTextDim, sc());
    }
    void mouse(const Event& e) override {
        if (e.mouse_kind == 3) { scroll -= e.wheel; return; }
        if (e.mouse_kind != 1) return;
        if (add_r().has(e.x, e.y)) { add(); return; }
        for (int k = 0; k < rows(); ++k) {
            const int idx = scroll + k;
            if (idx >= static_cast<int>(items.size())) break;
            const Rect r = row_r(k);
            if (!r.has(e.x, e.y)) continue;
            if (e.x > r.right() - 40 * sc()) items.erase(items.begin() + idx);
            else items[static_cast<std::size_t>(idx)].done = !items[static_cast<std::size_t>(idx)].done;
            save();
            return;
        }
    }
    void key(const Event& e) override {
        if (e.code == KEY_ENTER) { add(); return; }
        if (e.code == KEY_BACKSPACE) { if (!entry.empty()) entry.erase(u8_prev(entry, entry.size())); return; }
        if (!e.text.empty() && entry.size() < 200) entry += e.text;
    }
};

