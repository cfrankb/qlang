class Money {
   reserve = 0
   total =0

   # constructor
   __init__(self, amount)[] {
      self.total = amount
   }

   # destructor
   __destroy__(self)[] {
   }

   # string representation
   __str__(self)[] {
      return f"balance: {self.total}"
   }

   receive(self, amount)[] {
      self.total += amount
   }

   spend(self, amount)[] {
      self.total -= amount
   }

   balance(self)[] {
      return self.total
   }
 """
   setReserve(self, newTotal)[] {
      total = newTotal
   }

   getReserve(self)[] {
      return self.reserve
     # return Money::total
   }
"""
}

let money = Money(125)
money.receive(25)
println(f"balance: {money.balance()}")   # balance: 150
money.spend(15)
println(f"balance: {money.balance()}")   # balance: 135

let account = Money(13)
account.receive(100)
println(f"balance: {account.balance()}")   # balance: 113
println(f"balance: {money.balance()}")   # balance: 135

println(f"reserve: {money.total}")
#println(f"reserve: {money.getReserve()}")

println(money) # expecting output from _str__()
println(f"{money}") # expecting output from _str__()