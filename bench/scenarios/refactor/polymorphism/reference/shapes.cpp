#include "shapes.h"

class Shape {
public:
    virtual ~Shape() = default;
    virtual double area() const = 0;
};

class Circle : public Shape {
public:
    explicit Circle(double r) : r_(r) {}
    double area() const override { return 3.14159265358979323846 * r_ * r_; }

private:
    double r_;
};

class Square : public Shape {
public:
    explicit Square(double s) : s_(s) {}
    double area() const override { return s_ * s_; }

private:
    double s_;
};

double area_circle(double r) { return Circle(r).area(); }
double area_square(double s) { return Square(s).area(); }
