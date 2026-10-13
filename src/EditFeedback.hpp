// SPDX-License-Identifier: LGPL-3.0-or-later
// 编辑框差量朗读。宿主插入点以 UTF-8 字节计数，播报位置按字符计数。
#pragma once
#include "HostServices.hpp"
#include <algorithm>
#include <cmath>

class EditFeedback {
public:
    void Reset() { node = {}; text.clear(); caret = 0; }
    static size_t Caret(const std::string& text, double value) {
        if (!std::isfinite(value) || value <= 0) return 0;
        size_t pos = value >= double(text.size()) ? text.size() : size_t(value);
        while (pos && pos < text.size() && Continuation(text[pos])) --pos;
        return pos;
    }
    static std::string Position(const std::string& text, size_t pos) {
        if (pos == 0) return "插入点在开头";
        if (pos == text.size()) return "插入点在末尾";
        size_t count = 0;
        for (size_t i = 0; i < pos; ++i) if (!Continuation(text[i])) ++count;
        return "插入点在第" + std::to_string(count) + "个字符后";
    }
    static std::string Full(const std::string& title, const std::string& text, double value) {
        return (title.empty() ? "编辑框" : title + "，编辑框") +
            "，当前值：" + (text.empty() ? "空" : text) + "，" + Position(text, Caret(text, value));
    }
    std::string Update(TaoPluginAccessibleNodeV2 next, std::string current, double value,
        const std::string& title, bool announceEntry = true) {
        const size_t pos = Caret(current, value);
        std::string speech;
        if (!SameNode(node, next)) {
            if (announceEntry) speech = Full(title, current, value);
        } else if (text != current) {
            size_t first = 0;
            while (first < text.size() && first < current.size() && text[first] == current[first]) ++first;
            // 公共前缀可能止于汉字中间，回退到完整码点边界。
            while (first && ((first < text.size() && Continuation(text[first])) ||
                (first < current.size() && Continuation(current[first])))) --first;
            size_t oldEnd = text.size(), newEnd = current.size();
            while (oldEnd > first && newEnd > first && text[oldEnd - 1] == current[newEnd - 1]) {
                --oldEnd; --newEnd;
            }
            while (oldEnd < text.size() && Continuation(text[oldEnd])) ++oldEnd;
            while (newEnd < current.size() && Continuation(current[newEnd])) ++newEnd;
            if (oldEnd > first) speech = "删除 " + text.substr(first, oldEnd - first);
            if (newEnd > first) {
                if (!speech.empty()) speech += "，";
                speech += "输入 " + current.substr(first, newEnd - first);
            }
            if (!speech.empty()) speech += "，";
            speech += Position(current, pos);
        } else if (caret != pos) {
            // 左右移动读越过的字符；Home/End、Ctrl+方向键读越过的文本与新位置。
            const size_t begin = std::min(caret, pos), end = std::max(caret, pos);
            speech = current.substr(begin, end - begin) + "，" + Position(current, pos);
        }
        node = next; text = std::move(current); caret = pos;
        return speech;
    }

private:
    static bool Continuation(char c) { return (static_cast<unsigned char>(c) & 0xc0) == 0x80; }
    TaoPluginAccessibleNodeV2 node{};
    std::string text;
    size_t caret = 0;
};
