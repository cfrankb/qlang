# Control flow

## conditions

```python

if (a == 32) || (b==32) {

}


a = [1,4,6]
if (b in a) {

}

if (b in [5,7,4]) {

}

if b in [1,3,5] {

} else if b in [2,6,7] {

}

if (a == 4) {

} else {

}

# check if a between 1 and 10 (inclusive)
if a in range (1,10) {

}

if a == 1 return false;

if true return false

if true {

}
```

## range

```python
for x = 0 to 10 step 1 {
   ...
}

if i in range (0,10) {
   ...
}
```

rules
- step keyword is optional
- evaluate the relation ship between start and end value
- the direction determine the step (+1 or -1)
- enter the loop once
- continue to apply step until you are passed the end value

**guard rail.** if step is user specified and step is heading in the wrong direction then enter loop once before leaving
otherwise apply the same guard logic as above. 
step 0 creates an infinite loop similar to while true {}

infinite loops are not valid. implement a timeout guard that result in a runtime exception. make the message concise so that tracking the bug will be easier.

```python
# range keyword has different encoding based on context

arr = range(0, 10)

# encode: arr = [0,1,2,3,4,5,6,7,8,9,10]

for i in range (0, 10) {
   yield i
}

# encode: i = 0; while (i <= 10) { yield i; ++i }

while i in range (0,10) {
   ...
}

# encode: while i >=0 && i <= 10 {...}
# no increment here
# if i not defined throw error

let b = 3
let x = b in range(0,10)
# encode: x = (b >= 0) && (b <= 10)

if a in range (1,10) {
   ...
}

# encode if a >=1 && a <=10 { ... }

```

if used, the keyword `continue`  will apply the step. this is implicit

## switch

switch cases fallthrough until they reach break, return, goto or end of switch. 

```python
switch (var) {
   case "string2":
   case "string1":
      # var value is either "string1" or "string2" here
      break;
   case 123:
      break;
   case in [1,2,3]:
   case in range(10,20):   
   default:
}
```

## loops

the step keyword applies to ranges in for loops only and is explicitly not allowed when looping on a string or collection.

```python
for x = 1 to 32
next x

for x = 10 to 0 step -1
next x

for x in range (0,4)
next x

for x in range (10,0) step - 1
next x

for x in [1,25,6,7]
next x

# next can increment counter implicitely
# replicating the same behavior as basic

for x = 1 to 32
next # assume x

for y = 1 to 32
   for x = 1 to 32
   next # assume x
next # assume y

a = [1,4,6]
for x in a
next # assume x

for x in [1,4,6] step 2 {
   # error. you can't step into arrays
}


for i in range (10, 0) step - 2 {
}

for y in range (0,10) {
   for x in range (0,10) {
   }
}

for x = 1 to 32 {
   if x == 4 {
      continue # skip iteration
   }

   if (x == 2) {
      break #exit loop
   }
   printf("%d\n", x)
}

while (a > 0) {
 --a
}

a = [1,4,6]

for i in a {
   printf("i=%d\n", i) # can look up an array
}

for i in [4,6,8,9] {
   printf("i=%d\n", i) # can look up an array
}

for c in "string" {
   printf("char: %c\n", c)
}

# loops i from 1 to 10 (inclusive)
for i in range(1,10) {

}
```

## label / goto
```python
a = 10

:label1
--a

if a > 1 goto label1
```

label and goto must reside in the same scope.
if they cross a boundary, trigger an error.

