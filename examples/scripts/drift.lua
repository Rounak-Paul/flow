return {
    parameters = {
        distance = { type = "number", default = 16, min = 0, max = 2000 },
        speed = { type = "number", default = 0.7, min = 0, max = 20 },
    },
    sample = function(ctx)
        ctx.entity.pose.x = ctx.entity.pose.x + math.sin(ctx.time.seconds * ctx.params.speed * math.pi * 2) * ctx.params.distance
    end,
}
