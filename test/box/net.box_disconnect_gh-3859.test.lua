fiber = require 'fiber'
test_run = require('test_run').new()
net = require('net.box')

test_run:cmd('create server connecter with script = "box/proxy.lua"')

box.schema.func.create('fast_call')
box.schema.func.create('long_call')
box.schema.func.create('wait_signal')
box.schema.user.grant('guest', 'execute', 'function', 'fast_call')
box.schema.user.grant('guest', 'execute', 'function', 'long_call')
box.schema.user.grant('guest', 'execute', 'function', 'wait_signal')
c = net.connect(box.cfg.listen)

disconnected = false
function on_disconnect() disconnected = true end

-- Make sure all dangling connections are collected so
-- that on_disconnect trigger isn't called spuriously.
collectgarbage('collect')
fiber.sleep(0)

box.session.on_disconnect(on_disconnect) == on_disconnect

--
-- gh-3859: on_disconnect is called only after all requests are
-- processed, but should be called right after disconnect and
-- only once.
--
ch1 = fiber.channel(1)
ch2 = fiber.channel(1)
function wait_signal() ch1:put(true) ch2:get() end
_ = fiber.create(function() c:call('wait_signal') end)
ch1:get()

c:close()
fiber.sleep(0)
while disconnected == false do fiber.sleep(0.01) end
disconnected -- true
disconnected = nil

ch2:put(true)
fiber.sleep(0)
disconnected -- nil, on_disconnect is not called second time.

box.session.on_disconnect(nil, on_disconnect)

box.schema.func.drop('long_call')
box.schema.func.drop('fast_call')
box.schema.func.drop('wait_signal')
