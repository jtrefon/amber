#include "shapes.h"

enum ShapeType { CIRCLE, SQUARE };

struct Shape {
    ShapeType type;
    double a;
};

double shape_area(const Shape& s) {
    switch (s.type) {
        case CIRCLE:
            return 3.14159265358979323846 * s.a * s.a;
        case SQUARE:
            return s.a * s.a;
    }
    return 0.0;
}

double area_circle(double r) { return shape_area(Shape{CIRCLE, r}); }
double area_square(double s) { return shape_area(Shape{SQUARE, s}); }
