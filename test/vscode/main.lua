key = setmetatable({
    [1] = {1, 2, 3},
    ['size'] = {a=1,b=2},
}, {k = 0})
print(_VERSION)

function test(...)
    local n = ...
    print(_VERSION)
    key = 2
end
key = 4

test(1, "2s")
key = 5
