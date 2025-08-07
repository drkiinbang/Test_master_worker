#pragma once

#include "Point.h"

class Face {
 public:
  Face() = default;
  Face(Point normal, Point v1, Point v2, Point v3) :
        normal(normal),    v1(v1),    v2(v2),    v3(v3) {  }

  inline auto getNormal() const { return normal; }
  inline auto getv1() const { return v1; }
  inline auto getv2() const { return v2; }
  inline auto getv3() const { return v3; }

  inline auto getVertex(size_t idx) const {
    switch (idx) {
      case 0:return v1;
      case 1:return v2;
      case 2:return v3;
      default:return v3;
    }
  }

 protected:
 private:
  Point normal;
  Point v1;
  Point v2;
  Point v3;
};

