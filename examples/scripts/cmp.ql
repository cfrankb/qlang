a = 1 < 2 # BUG: should be bool, it returned int
println(f"1 < 2 {a} type={type(a)}")

a = 2 < 5 # BUG: should be bool, it returned int
println(f"2 < 5 {a} type={type(a)}")

a = 2 > 5 # BUG: should be bool, it returned int
println(f"2 > 5 {a} type={type(a)}")

a = "alana"
b = "betany"
c = a < b
println(f"{a} < {b} = {c} type={type(c)}")

c = a > b
println(f"{a} > {b} = {c} type={type(c)}")

c = a == b # BUG: should return bool, it returned it
println(f"{a} == {b} = {c} type={type(c)}")

a = [1,2,56,7]
b = [1,3,5]
c = a < b
println(f"{a} < {b} = {c} type={type(c)}")

c = a > b
println(f"{a} < {b} = {c} type={type(c)}")

c = a == a
println(f"{a} == {a} = {c} type={type(c)}")

c = a == b
println(f"{a} == {b} = {c} type={type(c)}")

c = a == b
println(f"{a} == {b} = {cast<int>(c)} type={type(c)}")



#printl("fdfdfd") # BUG: not triggering error, unknown printl