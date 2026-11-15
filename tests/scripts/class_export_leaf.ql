# The library for class_export.ql (class-export plan F4 P3b): a class with a
# constructor that reads a captured module const, a method, and a mutable
# table field default; plus a class without `__init__`. Not a test of its
# own — class_export.ql imports it.
export const MODULE_NAME = "class_export_leaf"
export const TAG = "leaf"

export class Point {
  x = 0
  y = 0
  tag = ""
  shared = []
  __init__(self, ax, ay) [TAG] {
    self.x = ax
    self.y = ay
    self.tag = TAG
  }
  sum(self)[] { return self.x + self.y }
}

export class Plain {
  flag = true
}
