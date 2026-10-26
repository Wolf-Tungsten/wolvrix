#ifndef WOLVRIX_EMIT_GRHSIM_FUNCTION_WRITER_HPP
#define WOLVRIX_EMIT_GRHSIM_FUNCTION_WRITER_HPP

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::emit::grhsim_detail {

// A structured C++ text writer, not a computation or scheduling representation.
// Scopes are extracted only at statement boundaries. Locals referred to by
// several resulting functions live in that source function's chunkFrame.
// Declaration-heavy indivisible statements can also spill locals to the frame.
struct Code {
    std::string text;
    std::vector<Code> body;
    bool scope = false;
    bool terminal = false;
    static Code line(std::string text, bool terminal = false);
    static Code block(std::string header, std::vector<Code> body);
    std::size_t lines() const;
};

struct Local {
    std::string type;
    std::string name;
    std::string initializer = "{}";
    bool parameter = false;
};

// Value-initialize with {} when fields require zero defaults. The explicitly
// defaulted constructor avoids large aggregate expansions. Functions whose
// locals are all assigned before use can instead default-initialize the frame.
std::string writeValueStruct(std::string_view name, const std::vector<Local>& fields);

struct ValueStructLayout {
    std::string declaration;
    std::map<std::string, std::string> references;
    std::vector<Local> physicalFields;
};

// Large frames use typed arrays instead of millions of distinct C++ members.
// Explicit initializers keep their own member and initialization.
ValueStructLayout writeValueLayout(std::string_view name, const std::vector<Local>& fields);

// Named stores are part of the generated model ABI. Keep every semantic field
// as a direct member so store layout remains inspectable and independent from
// the spill layout used by chunk frames.
ValueStructLayout writeDirectValueLayout(std::string_view name, const std::vector<Local>& fields);

struct Function {
    std::string name;
    std::string parameters;
    std::string result = "void";
    bool isConst = false;
    std::vector<Local> locals;
    std::vector<Code> body;
    // Only opt out when the producer assigns every local before its use,
    // including across guarded chunks. Explicit initializers still apply.
    bool zeroInitializeLocals = true;
};

struct WrittenFunction {
    std::string name;
    std::string declaration;
    std::string definition;
    std::size_t lines = 0;
};

struct WrittenFunctions {
    std::string frameDeclaration;
    std::vector<WrittenFunction> functions;
};

// Local references in Code use @name@. They resolve either to a local or to a
// chunkFrame field after splitting and liveness analysis have reached a fixpoint.
WrittenFunctions writeFunction(const Function& function, std::string_view className,
                               std::size_t maxLines);

}
#endif
