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
      return f"<>money: {self.total}"
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
}

let money = Money(125)
money.receive(25)
println(f"balance: {money.balance()}")   # balance: 150
money.spend(15)
println(f"balance: {money.balance()}")   # balance: 135

let account = Money(13)
account.receive(100)
println(f"account: {account.balance()}")   # account: 113
println(f"balance: {money.balance()}")   # balance: 135

println(money) # expecting output from _str__()
println(f"{money}") # expecting output from _str__()