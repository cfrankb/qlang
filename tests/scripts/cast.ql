local a = 1
a = cast<float>(a)
println(f"a = {a} type: {type(a)} len {len(a)}") 
# expected: a = 1.0

local a = 12
a = cast<string>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}")
# expected: a = 12

local a = 12
a = cast<str>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}")
# expected: a = 12

local a = 1.24
println(f"a = {a} type: {type(a)} len: {len(a)}") 
# expected: a = 1.24

local a = 1.24
#a = cast<array>(a)
#this is correct: cannot cast to array
a = cast<dword>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}")

a = cast<qword>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}") 
# expected: a = 1

local a = -1
a = cast<dword>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}") 
# expected: 4294967295
# dword is unsigned

local a = -1
a = cast<qword>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}") 
# expected: 18446744073709551615

local a = -1
a = cast<word>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}") 
# expected: 65535
# word is unsigned

a = cast<char>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}") 
# expected: -1

a = cast<short>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}") 

a = cast<int>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}") 

a = cast<long>(a)
println(f"a = {a} type: {type(a)} len: {len(a)}") 
