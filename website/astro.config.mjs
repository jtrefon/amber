import { defineConfig } from 'astro/config';
import sitemap from '@astrojs/sitemap';

export default defineConfig({
  site: 'https://jtrefon.github.io/amber',
  base: '/amber/',
  output: 'static',
  integrations: [sitemap()],
});
