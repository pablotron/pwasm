#!/usr/bin/env ruby

require 'csv'

def fib(n)
  (n >= 2) ? (fib(n - 1) + fib(n - 2)) : 1
end

CSV(STDOUT) do |csv|
  csv << %w{num fib}
  10.times do |i|
    csv << [i, fib(i)]
  end
end
