#include "Expression.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <limits>

class Expression::Parser {
public:
    Parser(std::string_view s, std::string& e) : text(s), error(e) {}

    std::unique_ptr<Node> parse() {
        auto n = expression();
        skip();
        if (!n) return nullptr;
        if (pos != text.size()) {
            fail("unexpected character at position " + std::to_string(pos));
            return nullptr;
        }
        return n;
    }

private:
    std::string_view text;
    std::string& error;
    size_t pos = 0;

    void skip() { while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos; }
    bool eat(char c) { skip(); if (pos < text.size() && text[pos] == c) { ++pos; return true; } return false; }
    void fail(const std::string& e) { if (error.empty()) error = e; }

    std::unique_ptr<Node> expression() {
        auto left = term();
        if (!left) return nullptr;
        for (;;) {
            skip();
            if (eat('+')) { auto r = term(); if (!r) return nullptr; left = binary('+', std::move(left), std::move(r)); }
            else if (eat('-')) { auto r = term(); if (!r) return nullptr; left = binary('-', std::move(left), std::move(r)); }
            else break;
        }
        return left;
    }

    std::unique_ptr<Node> term() {
        auto left = power();
        if (!left) return nullptr;
        for (;;) {
            skip();
            if (eat('*')) { auto r = power(); if (!r) return nullptr; left = binary('*', std::move(left), std::move(r)); }
            else if (eat('/')) { auto r = power(); if (!r) return nullptr; left = binary('/', std::move(left), std::move(r)); }
            else break;
        }
        return left;
    }

    std::unique_ptr<Node> power() {
        auto left = unary();
        if (!left) return nullptr;
        skip();
        if (eat('^')) {
            auto right = power();
            if (!right) return nullptr;
            return binary('^', std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Node> unary() {
        skip();
        if (eat('+')) return unary();
        if (eat('-')) {
            auto n = std::make_unique<Node>();
            n->kind = Node::Kind::Unary;
            n->op = '-';
            n->right = unary();
            if (!n->right) { fail("expected expression after '-'"); return nullptr; }
            return n;
        }
        return primary();
    }

    std::unique_ptr<Node> primary() {
        skip();
        if (eat('(')) {
            auto n = expression();
            if (!eat(')')) { fail("missing ')' "); return nullptr; }
            return n;
        }
        if (pos >= text.size()) { fail("unexpected end of expression"); return nullptr; }

        if (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '.') {
            const char* begin = text.data() + pos;
            char* end = nullptr;
            double value = std::strtod(begin, &end);
            pos += static_cast<size_t>(end - begin);
            auto n = std::make_unique<Node>();
            n->kind = Node::Kind::Number;
            n->value = value;
            return n;
        }

        if (std::isalpha(static_cast<unsigned char>(text[pos]))) {
            size_t start = pos;
            while (pos < text.size() && (std::isalpha(static_cast<unsigned char>(text[pos])) || std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) ++pos;
            std::string name(text.substr(start, pos - start));
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

            if (lower == "x" || lower == "a" || lower == "b" || lower == "c") {
                auto n = std::make_unique<Node>();
                n->kind = Node::Kind::Variable;
                n->variable = lower[0];
                return n;
            }
            if (lower == "pi") {
                auto n = std::make_unique<Node>(); n->kind = Node::Kind::Number; n->value = 3.14159265358979323846; return n;
            }
            if (lower == "e") {
                auto n = std::make_unique<Node>(); n->kind = Node::Kind::Number; n->value = 2.71828182845904523536; return n;
            }

            if (!eat('(')) { fail("unknown identifier: " + name); return nullptr; }
            auto arg = expression();
            if (!eat(')')) { fail("missing ')' after " + name); return nullptr; }
            static const char* funcs[] = {"sin","cos","tan","asin","acos","atan","sqrt","abs","log","ln","exp","floor","ceil"};
            bool ok = false; for (auto f : funcs) if (lower == f) { ok = true; break; }
            if (!ok) { fail("unknown function: " + name); return nullptr; }
            auto n = std::make_unique<Node>(); n->kind = Node::Kind::Function; n->function = lower; n->left = std::move(arg); return n;
        }

        fail("unexpected character at position " + std::to_string(pos));
        return nullptr;
    }

    std::unique_ptr<Node> binary(char op, std::unique_ptr<Node> a, std::unique_ptr<Node> b) {
        auto n = std::make_unique<Node>(); n->kind = Node::Kind::Binary; n->op = op; n->left = std::move(a); n->right = std::move(b); return n;
    }
};

bool Expression::parse(std::string_view source, std::string& error) {
    error.clear(); root_.reset(); Parser p(source, error); root_ = p.parse(); return root_ != nullptr;
}

double Expression::evaluate(const Node* n, double x, const std::array<double, 3>& coefficients) {
    if (!n) return std::numeric_limits<double>::quiet_NaN();
    switch (n->kind) {
        case Node::Kind::Number: return n->value;
        case Node::Kind::Variable:
            if (n->variable == 'x') return x;
            if (n->variable == 'a') return coefficients[0];
            if (n->variable == 'b') return coefficients[1];
            if (n->variable == 'c') return coefficients[2];
            return std::numeric_limits<double>::quiet_NaN();
        case Node::Kind::Unary: return -evaluate(n->right.get(), x, coefficients);
        case Node::Kind::Binary: {
            double a = evaluate(n->left.get(), x, coefficients), b = evaluate(n->right.get(), x, coefficients);
            switch (n->op) { case '+': return a+b; case '-': return a-b; case '*': return a*b; case '/': return std::abs(b) < 1e-12 ? std::numeric_limits<double>::quiet_NaN() : a/b; case '^': return std::pow(a,b); }
        }
        case Node::Kind::Function: {
            double a = evaluate(n->left.get(), x, coefficients);
            if (!std::isfinite(a)) return a;
            if (n->function == "sin") return std::sin(a);
            if (n->function == "cos") return std::cos(a);
            if (n->function == "tan") return std::tan(a);
            if (n->function == "asin") return std::asin(a);
            if (n->function == "acos") return std::acos(a);
            if (n->function == "atan") return std::atan(a);
            if (n->function == "sqrt") return a < 0 ? std::numeric_limits<double>::quiet_NaN() : std::sqrt(a);
            if (n->function == "abs") return std::abs(a);
            if (n->function == "log" || n->function == "ln") return a <= 0 ? std::numeric_limits<double>::quiet_NaN() : std::log(a);
            if (n->function == "exp") return std::exp(a);
            if (n->function == "floor") return std::floor(a);
            if (n->function == "ceil") return std::ceil(a);
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

double Expression::eval(double x) const { return evaluate(root_.get(), x, {1.0, 0.0, 0.0}); }

double Expression::eval(double x, const std::array<double, 3>& coefficients) const {
    return evaluate(root_.get(), x, coefficients);
}
