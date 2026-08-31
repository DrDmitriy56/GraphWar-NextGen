#pragma once
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>

class Expression {
public:
    Expression() = default;
    bool parse(std::string_view source, std::string& error);
    double eval(double x) const;
    double eval(double x, const std::array<double, 3>& coefficients) const;
    bool valid() const { return root_ != nullptr; }

private:
    struct Node {
        enum class Kind { Number, Variable, Unary, Binary, Function } kind{};
        double value{};
        char op{};
        char variable{};
        std::string function;
        std::unique_ptr<Node> left;
        std::unique_ptr<Node> right;
    };

    std::unique_ptr<Node> root_;

    class Parser;
    static double evaluate(const Node* node, double x, const std::array<double, 3>& coefficients);
};
