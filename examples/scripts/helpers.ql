let a = 10
println(f"var a = {a}; type={type(a)}")
printf(f"var a = {a}; type={type(a)}") # BUG: this line doesn't print anything
print(f"var a = {a}; type={type(a)}") # this line prints
printf(f"var a = {a}; type={type(a)}\n") # this line prints
println(f"1 type={type(1)}")
println(f"[] type={type([])}")

let b = [1,2,3]
let l = len(b)
println(f"var b = {b} type={type(b)} len={l}") # 
#fails as expected 
#println(f"var b = {$b} type={type([])}")

let c = {name="Bob", age=99}
println(f"name={c.name} age={c.age} type={type(c)} len={len(c)}")

let f = "age"
let d = {name="Anna", $f=$a}
println(f"name={d.name} age={d.age} type={type(d)}")

let name = "Albert The Block"
println("name substr[0:6] = " + substr(name, 0, 6))

let arr = [1,5,7,10,4,6]
let slc = slice(arr, 0, 4)
println(f"slc len={len(slc)}")

# isset is a keyword: a bare identifier arg compiles to its name string,
# so undeclared variables probe as false instead of a compile error
println(f"isset(xx) {isset(xx)}")
println(f"isset(a) {isset(a)}")
println(f"true {true}")
println(f"nil {nil}")

bb = true
println(f"bb {true}")

printf("this is just a line")
println(f""" TRIPPLE QUOTE join arr = {join(",", arr)}""")

jj = join(", ", arr)

println(f"""join arr jj = {jj}""") 
println(jj)
