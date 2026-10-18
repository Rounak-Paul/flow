--- Initialize the owning entity's orbit parameters and center.
function on_init(self)
    self.center_x = self.transform.x
    self.center_y = self.transform.y
    self.radius = 120
    self.speed = 1.5
end

--- Update the orbit at time seconds; dt is the fixed simulation step.
function on_update(self, dt, time)
    local angle = time * self.speed
    self.transform.x = self.center_x + math.cos(angle) * self.radius
    self.transform.y = self.center_y + math.sin(angle) * self.radius
    self.transform.rotation = angle
end
