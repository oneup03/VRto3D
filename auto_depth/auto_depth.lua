local pipeName = "\\\\.\\pipe\\AutoDepth"

-- Oscillation logic
local value = 0.5
local decreasing = true

local function updateValue()
    if decreasing then
        value = value - 0.1
        if value <= 0.1 then
            decreasing = false
        end
    else
        value = value + 0.1
        if value >= 0.5 then
            decreasing = true
        end
    end
    return string.format("%.2f", value) -- Format as a string with 2 decimal places
end

while true do
    -- Try to open the named pipe for writing
    local pipe = io.open(pipeName, "w")

    if not pipe then
        print("Waiting for C++ server to start...")
        os.execute("timeout 1 > nul") -- Wait 1 second before retrying (Windows-compatible)
    else
        print("Connected to named pipe! Sending values...")

        -- Continuously write oscillating values
        while true do
            local oscillatingValue = updateValue()
            
            -- Write value to the named pipe
            pipe:write(oscillatingValue .. "\n")
            pipe:flush()

            print("Sent:", oscillatingValue)

            -- Sleep for 100ms before next update
            os.execute("timeout 1 > nul") -- Windows-compatible sleep
        end

        -- Close the pipe
        pipe:close()
    end
end
