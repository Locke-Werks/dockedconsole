// Docked Console, a terminal docked to the edge of the Windows desktop.
// Copyright (C) 2026 Locke Werks
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dock::json {

/// One value from the config file. The config is a flat object, so there is no
/// nesting to model: the only compound value is an array of strings.
struct Value {
    enum class Type { Null, Bool, Number, String, Array };

    Type type = Type::Null;
    bool boolean = false;
    long long number = 0;
    std::wstring string;
    std::vector<std::wstring> array;
};

/// Key/value pairs in file order. A vector rather than a map because the file is
/// ten keys long and round-tripping wants the original order anyway.
using Object = std::vector<std::pair<std::wstring, Value>>;

/// Parses a flat JSON object.
///
/// Deliberately more forgiving than RFC 8259, matching what System.Text.Json was
/// configured to accept in the C# version: `//` and block comments are skipped
/// and a trailing comma before the closing brace or bracket is allowed. That
/// tolerance is the reason this is hand-written; nlohmann/json is strict and
/// offers no trailing-comma option, so adopting it would have quietly broken
/// config files that work today.
///
/// Returns false and fills `error` with a human-readable message including a
/// line number. The caller is expected to report that and carry on with
/// defaults rather than overwrite the file, because the file is the only copy of
/// something a person typed.
bool ParseObject(std::wstring_view text, Object& out, std::wstring& error);

/// Finds a key, case-sensitively. Null when absent.
[[nodiscard]] const Value* Find(const Object& object, std::wstring_view key);

/// Escapes and quotes a string for output. Escapes the two characters JSON
/// requires plus the control range; notably `\` is escaped, which is what makes
/// a monitor device name such as \\.\DISPLAY2 survive a round trip.
[[nodiscard]] std::wstring Quote(std::wstring_view value);

} // namespace dock::json
