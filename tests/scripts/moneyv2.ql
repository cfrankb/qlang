class Money {
   static const prefix = "balance"
   static banner = "GreatAccount"
   total = 0

   # constructor
   __init__(self, amount)[] {
      self.total = amount
   }

   # destructor
   __destroy__(self)[] {
   }

   # string representation
   __str__(self)[] {
      return f"{prefix}: {self.total} at {banner}"
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

   static getBanner()[] {
      return banner;
   }
   static setBanner(newBanner)[] {
      banner = newBanner;
   }
}

let money = Money(125)
money.receive(25)
println(f"balance: {money.balance()}")   # balance: 150
money.spend(15)
println(f"balance: {money.balance()}")   # balance: 135

println(money) # expected: output from _str__()
println(f"{money}") # expected: output from _str__()

Money::setBanner("OrangePumpkin")
println(Money::getBanner()) # expected: "OrangePumpkin"
println(f"{Money::banner}") # expected: "OrangePumpkin"

println(f"banner: {money.banner}") #expected: "OrangePumpkin"
money.banner = "Turtle"
println(f"banner: {money.banner}") #expected: "Turtle"
println(f"banner: {Money::banner}") #expected: "Turtle"

# expected to fail
#money.prefix = "toto"