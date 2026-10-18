--- Orbit the entity's authored position; the offset is a pure function of time.
function on_evaluate(self, time)
    local angle = time * 1.5
    self.transform.x = self.transform.x + math.cos(angle) * 120
    self.transform.y = self.transform.y + math.sin(angle) * 120
    self.transform.rotation = angle
end
