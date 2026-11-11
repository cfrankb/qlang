# A module-level const that the module's own `def` reads. Not a test of its
# own — ql_module_const.ql imports it (F1, module-export/import plan P0).
export const MODULE_NAME = "ql_module_const_dep"

export const TAG = "tagged"

export def tag()
  return "fn:" + TAG
end
