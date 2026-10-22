import { createApp } from 'vue'
import App from './App.vue'
import { router } from './router'
import './stores/theme'
import './styles/tokens.css'
import './styles/base.css'
import './styles/controls.css'

createApp(App).use(router).mount('#app')
