-- Repeatedly exercises the render-hook lifecycle while DF continues rendering.
-- Run with: script test_lifecycle_stress.lua [cycles]

local cycles = tonumber(({...})[1]) or 100
if cycles < 1 then
    qerror('cycles must be a positive integer')
end
cycles = math.floor(cycles)

for _ = 1, cycles do
    dfhack.run_command('enable', 'smooth-movement')
    dfhack.run_command('disable', 'smooth-movement')
end

for _ = 1, cycles do
    dfhack.run_command('enable', 'smooth-movement')
    dfhack.run_command('unload', 'smooth-movement')
    dfhack.run_command('load', 'smooth-movement')
end

dfhack.run_command('enable', 'smooth-movement')
print(('smooth-movement lifecycle stress completed: %d cycles per phase'):format(cycles))
