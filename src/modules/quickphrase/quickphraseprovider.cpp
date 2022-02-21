/*
 * SPDX-FileCopyrightText: 2020-2020 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
#include "quickphraseprovider.h"
#include <fstream>
#include <fcntl.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputmethodentry.h>
#include "fcitx-utils/standardpath.h"
#include "fcitx-utils/stringutils.h"
#include "quickphrase.h"
#include "spell_public.h"

namespace fcitx {

bool BuiltInQuickPhraseProvider::populate(
    InputContext *, const std::string &userInput,
    const QuickPhraseAddCandidateCallback &addCandidate) {
    auto start = map_.lower_bound(userInput);
    auto end = map_.end();

    for (; start != end; start++) {
        if (!stringutils::startsWith(start->first, userInput)) {
            break;
        }
        addCandidate(start->second,
                     stringutils::concat(start->second, " ",
                                         start->first.substr(userInput.size())),
                     QuickPhraseAction::Commit);
    }
    return true;
}
void BuiltInQuickPhraseProvider::reloadConfig() {

    map_.clear();
    auto file = StandardPath::global().open(StandardPath::Type::PkgData,
                                            "data/QuickPhrase.mb", O_RDONLY);
    auto files = StandardPath::global().multiOpen(
        StandardPath::Type::PkgData, "data/quickphrase.d/", O_RDONLY,
        filter::Suffix(".mb"));
    auto disableFiles = StandardPath::global().multiOpen(
        StandardPath::Type::PkgData, "data/quickphrase.d/", O_RDONLY,
        filter::Suffix(".mb.disable"));
    if (file.fd() >= 0) {
        load(file);
    }

    for (auto &p : files) {
        if (disableFiles.count(stringutils::concat(p.first, ".disable"))) {
            continue;
        }
        load(p.second);
    }
}

void BuiltInQuickPhraseProvider::load(StandardPathFile &file) {
    UniqueFilePtr fp{fdopen(file.fd(), "rb")};
    if (!fp) {
        return;
    }
    file.release();

    UniqueCPtr<char> buf;
    size_t len = 0;
    while (getline(buf, &len, fp.get()) != -1) {
        std::string strBuf(buf.get());

        auto pair = stringutils::trimInplace(strBuf);
        std::string::size_type start = pair.first, end = pair.second;
        if (start == end) {
            continue;
        }
        std::string text(strBuf.begin() + start, strBuf.begin() + end);
        if (!utf8::validate(text)) {
            continue;
        }

        auto pos = text.find_first_of(FCITX_WHITESPACE);
        if (pos == std::string::npos) {
            continue;
        }

        auto word = text.find_first_not_of(FCITX_WHITESPACE, pos);
        if (word == std::string::npos) {
            continue;
        }

        if (text.back() == '\"' &&
            (text[word] != '\"' || word + 1 != text.size())) {
            continue;
        }

        std::string key(text.begin(), text.begin() + pos);
        std::string wordString;

        bool escapeQuote;
        if (text.back() == '\"' && text[word] == '\"') {
            wordString = text.substr(word + 1, text.size() - word - 1);
            escapeQuote = true;
        } else {
            wordString = text.substr(word);
            escapeQuote = false;
        }
        stringutils::unescape(wordString, escapeQuote);

        map_.emplace(std::move(key), std::move(wordString));
    }
}

SpellQuickPhraseProvider::SpellQuickPhraseProvider(QuickPhrase *quickPhrase)
    : parent_(quickPhrase), instance_(parent_->instance()) {}

bool SpellQuickPhraseProvider::populate(
    InputContext *ic, const std::string &userInput,
    const QuickPhraseAddCandidateCallback &addCandidate) {
    if (!*parent_->config().enableSpell) {
        return true;
    }
    auto spell = this->spell();
    if (!spell) {
        return true;
    }
    std::string lang = *parent_->config().fallbackSpellLanguage;
    if (auto entry = instance_->inputMethodEntry(ic)) {
        if (spell->call<ISpell::checkDict>(entry->languageCode())) {
            lang = entry->languageCode();
        } else if (!spell->call<ISpell::checkDict>(lang)) {
            return true;
        }
    }
    const auto result = spell->call<ISpell::hint>(
        lang, userInput, instance_->globalConfig().defaultPageSize());
    for (const auto &word : result) {
        addCandidate(word, word, QuickPhraseAction::Commit);
    }
    return true;
}

bool CallbackQuickPhraseProvider::populate(
    InputContext *ic, const std::string &userInput,
    const QuickPhraseAddCandidateCallback &addCandidate) {
    for (const auto &callback : callback_.view()) {
        if (!callback(ic, userInput, addCandidate)) {
            return false;
        }
    }
    return true;
}

struct ArithParser {
    const std::string &input;
    size_t pos;

    /*
    expr -> expr [- +] term | term
    term -> term [/ *] factor | factor
    factor -> (expr) | number
    */

    std::optional<double> factor() {
        if (pos >= input.size()) { return std::nullopt; }

        if (input[pos] == '(') {
            pos++;
            auto expr = this->expr();
            if (!expr || pos >= input.size() || input[pos] != ')') {
                return std::nullopt;
            }
            pos++;
            return expr.value();
        }

        auto start = pos;
        bool decimal = false;

        while (pos < input.size() && (isdigit(input[pos]) || (input[pos] == '.' && !decimal))) {
            decimal |= input[pos] == '.';
            pos++;
        }

        if (pos == start) { return std::nullopt; }
        try {
            return std::stod(input.substr(start, pos - start));
        } catch(...) {
            return std::nullopt;
        }
    }

    std::optional<double> term() {
        auto _pos = pos;
        auto left = this->factor();
        if (!left) { return std::nullopt; }
        else if (pos >= input.size()) { return left; }

        while (true) {
            if (input[pos] == '*') {
                pos++;
                auto right = this->factor();
                if (!right) { return std::nullopt; }
                left = left.value() * right.value();
            } else if (input[pos] == '/') {
                pos++;
                auto right = this->factor();
                if (!right) { return std::nullopt; }
                left = left.value() / right.value();
            } else { break; }
        }
        return left.value();
    }

    std::optional<double> expr() {
        auto _pos = pos;
        auto left = term();
        if (!left) { return std::nullopt; }
        else if (pos >= input.size()) { return left; }

        while (true) {
            if (input[pos] == '+') {
                pos++;
                auto right = this->term();
                if (!right) { return std::nullopt; }
                left = left.value() + right.value();
            } else if (input[pos] == '-') {
                pos++;
                auto right = this->term();
                if (!right) { return std::nullopt; }
                left = left.value() - right.value();
            } else { break; }
        }
        return left.value();
    }

    std::optional<int> skipSpaces() {
        auto start = pos;
        while (pos < input.size() && std::isspace(input[pos])) {
            pos++;
        }
        return pos - start;
    }
};

bool CalcQuickPhraseProvider::populate(
    InputContext *ic, const std::string &userInput,
    const QuickPhraseAddCandidateCallback &addCandidate) {
    // evaluate userInput as an arithmetic expression
    auto parser = ArithParser { userInput, 0 };
    auto expr = parser.expr();
    if (expr.has_value()) {
        auto n = std::to_string(expr.value());
        addCandidate(n, n, QuickPhraseAction::NoneSelectionCommit);
    }
    return true;
}

bool PandocQuickPhraseProvider::populate(
    InputContext *ic, const std::string &userInput,
    const QuickPhraseAddCandidateCallback &addCandidate) {

    if (userInput.find("pd:") != 0) { return true; }

    std::string result;
    std::string path = "/tmp/quickphrase-pandoc-";
    path += std::to_string(getpid());

    {
        // FIXME awful workaround for C++'s lack of proper subprocess handling
        std::string cmd = "/usr/bin/pandoc -f latex - -t plain > ";
        cmd += path;

        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "w"), pclose);
        if (!pipe) {
            std::cout << "could not execute pandoc: " << strerror(errno) << std::endl;
            return true;
        }
        // userInput to stdin
        std::cout << "passing " << userInput << std::endl;
        fputs("$", pipe.get());
        fputs(userInput.substr(3, userInput.length() - 1).c_str(), pipe.get());
        fputs("$\n", pipe.get());
        fflush(pipe.get());
    }

    // read the file at path into result
    std::ifstream ifs(path, std::ios::in);
    if (!ifs) {
        std::cout << "could not open " << path << std::endl;
        return true;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        result += line;
    }

    addCandidate(result, result, QuickPhraseAction::NoneSelectionCommit);

    return true;
}

} // namespace fcitx
