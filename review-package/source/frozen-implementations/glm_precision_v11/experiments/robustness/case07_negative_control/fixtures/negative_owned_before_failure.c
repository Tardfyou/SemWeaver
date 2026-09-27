struct match_template { void *fc; };
struct context { int unused; };

void kfree(void *pointer);
void *allocate_fc(void);

int hws_definer_conv_match_params_to_hl(struct context *ctx,
                                        struct match_template *mt,
                                        void *match_hl)
{
    (void)ctx;
    (void)match_hl;
    mt->fc = allocate_fc();
    return mt->fc ? -1 : 0;
}

int mlx5hws_definer_calc_layout(struct context *ctx,
                                struct match_template *mt,
                                void *match_hl)
{
    int ret;
    ret = hws_definer_conv_match_params_to_hl(ctx, mt, match_hl);
    if (ret)
        goto free_fc;

free_fc:
    kfree(mt->fc);
free_match_hl:
    kfree(match_hl);
    return ret;
}
