--- Initialize the owning entity; the Animation graph defines Idle/Run and a speed parameter.
function on_init(self)
    self.speed = 120
    self.animation:play("Idle")
end

--- Move the entity and let the graph choose motion states from the speed parameter.
function on_update(self, dt, time)
    self.transform.x = self.transform.x + self.speed * dt
    self.animation:parameter("speed", self.speed)
end
