return {
    parameters = {
        distance = { type = "number", default = 48, min = 0, max = 2000 },
        duration = { type = "number", default = 0.6, min = 0.05, max = 8 },
    },
    sample = function(ctx)
        local duration = ctx.params.duration
        if duration <= 0 then duration = 0.6 end
        local t = math.min(1, ctx.time.seconds / duration)
        local settled = 1 - (1 - t) * (1 - t)
        ctx.entity.pose.y = ctx.entity.pose.y + (1 - settled) * ctx.params.distance
        ctx.entity.opacity = settled
        if ctx.entity.motion then
            ctx.entity.motion.rise = (1 - settled) * ctx.params.distance
            ctx.entity.motion.fade = 1 - settled
        end
    end,
}
