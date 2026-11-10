# A module with no image on disk: plan C8b compiles this `.ql` when something
# imports the name it declares. Not a test of its own — ql_module.ql imports it.
export const MODULE_NAME = "ql_module_dep"

export def shout(name)
  return "HELLO " + name
end
