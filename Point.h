//#ifndef CHANGE_DECTION_SRC_DOMAIN_POINT_H_
//#define CHANGE_DECTION_SRC_DOMAIN_POINT_H_
#pragma once

#include <iostream>
#include <cmath>

class Point {

public:
    enum { X, Y, Z };
    /*Point();
    Point(float x, float y, float z);
    Point(float* coords);
    Point(const Point& other);*/
    Point() {
      coords[X] = 0.0;
      coords[Y] = 0.0;
      coords[Z] = 0.0;
    }
    Point(float x, float y, float z) {
      coords[X] = x;
      coords[Y] = y;
      coords[Z] = z;
    }

    Point(float* coords) {
      memcpy(this->coords, coords, sizeof(float) * 3);
    }

    Point(const Point& other) {
      assign(other);
    }

    

    float* getCoords() { return coords; }

    inline auto getX() const { return coords[X]; }
    inline auto getY() const { return coords[Y]; }
    inline auto getZ() const { return coords[Z]; }

    inline void setX(float x) { coords[X] = x;  }
    inline void setY(float y) { coords[Y] = y;  }
    inline void setZ(float z) { coords[Z] = z;  }

    inline void move(float offset) {
      coords[X] += offset;
      coords[Y] += offset;
      coords[Z] += offset;
    }

    inline void move(float* offsets) {
        coords[X] -= offsets[0];
        coords[Y] -= offsets[1];
        coords[Z] -= offsets[2];
    }

    inline bool isEmpty() {
      if (coords[X] == 0.0 && coords[Y] == 0.0 && coords[Z] == 0.0) {
        return true;
      } else {
        return false;
      }
    }

    inline float at(size_t idx) const {
      if (idx > Z) {
        std::cerr << "index over Z" << std::endl;
        return 0.0;
      }
      return coords[idx];
    }

    Point& operator=(const Point& other) {
      if (this == &other) {
        return *this;
      }
      assign(other);
      return *this;
    }  


    // Vector addition
    Point operator+(const Point& other) const {
      return Point(coords[X] + other.coords[X], coords[Y] + other.coords[Y], coords[Z] + other.coords[Z]);
    }

    // Vector subtraction
    Point operator-(const Point& other) const {
      return Point(coords[X] - other.coords[X], coords[Y] - other.coords[Y], coords[Z] - other.coords[Z]);
    }

    // Vector cross product
    Point crossProduct(const Point& other) const {
      return Point(
        coords[Y] * other.coords[Z] - coords[Z] * other.coords[Y],
        coords[Z] * other.coords[X] - coords[X] * other.coords[Z],
        coords[X] * other.coords[Y] - coords[Y] * other.coords[X]
      );
    }

    // Vector dot product
    float dotProduct(const Point& other) const {
      return coords[X] * other.coords[X] + coords[Y] * other.coords[Y] + coords[Z] * other.coords[Z];
    }

    // Vector normalization
    Point normalize() const {
      float length = std::sqrt(coords[X] * coords[X] + coords[Y] * coords[Y] + coords[Z] * coords[Z]);
      return Point(coords[X] / length, coords[Y] / length, coords[Z] / length);
    }

   protected:
     void assign(const Point& other) {
       memcpy(coords, other.coords, sizeof(float) * 3);
     }

   private:
    float coords[3];
};

//#endif //CHANGE_DECTION_SRC_DOMAIN_POINT_H_
