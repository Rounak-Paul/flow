return {
    parameters = {
        distance = { type = "number", default = 48, min = 0, max = 2000 },
        hold = { type = "number", default = 1.2, min = 0, max = 120 },
        duration = { type = "number", default = 0.45, min = 0.05, max = 8 },
    },
    sample = function(ctx)
        local duration = ctx.params.duration
        if duration <= 0 then duration = 0.45 end
        local t = math.max(0, math.min(1, (ctx.time.seconds - ctx.params.hold) / duration))
        local gone = t * t
        ctx.entity.pose.y = ctx.entity.pose.y - gone * ctx.params.distance
        ctx.entity.opacity = 1 - gone
        if ctx.entity.motion then
            ctx.entity.motion.rise = -gone * ctx.params.distance
            ctx.entity.motion.fade = gone
        end
    end,
}
