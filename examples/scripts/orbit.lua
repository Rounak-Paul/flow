return {
    parameters = {
        radius = { type = "number", default = 100, min = 0 },
        speed = { type = "number", default = 1 },
    },
    sample = function(ctx)
        local angle = ctx.time.seconds * ctx.params.speed
        ctx.entity.transform.x = ctx.entity.transform.x + math.cos(angle) * ctx.params.radius
        ctx.entity.transform.y = ctx.entity.transform.y + math.sin(angle) * ctx.params.radius
    end,
}
